#define _POSIX_C_SOURCE 200809L

#include "userfs.h"
#include "khalid_structs.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static FILE *disk = NULL;
static struct ufs_superblock sb;
static struct ufs_open_file open_files[UFS_MAX_OPEN_FILES];

/* Copies in RAM of the on-disk allocation maps. */
static uint8_t *inode_bitmap = NULL;
static uint8_t *block_bitmap;

static void bitmap_set(uint8_t *bitmap, uint64_t index)
{
    bitmap[index / 8] |= (uint8_t)(1u << (index % 8));
}

static void bitmap_clear(uint8_t *bitmap, uint64_t index)
{
    bitmap[index / 8] &= (uint8_t)~(1u << (index % 8));
}

static int bitmap_test(const uint8_t *bitmap, uint64_t index)
{
    return (bitmap[index / 8] >> (index % 8)) & 1u;
}

static uint64_t bitmap_blocks_needed(uint64_t count)
{
    return (count + UFS_BITS_PER_BITMAP_BLOCK - 1) /
           UFS_BITS_PER_BITMAP_BLOCK;
}

static int64_t bitmap_find_free(const uint8_t *bitmap, uint64_t count)
{
    uint64_t i;

    for (i = 0; i < count; ++i)
    {
        if (!bitmap_test(bitmap, i))
        {
            return (int64_t)i;
        }
    }

    return -1;
}

static int disk_read_block(uint64_t block_num, void *buf)
{
    off_t offset;

    if (disk == NULL)
    {
        errno = EBADF;
        return -1;
    }

    if (sb.total_blocks != 0 && block_num >= sb.total_blocks)
    {
        errno = EINVAL;
        return -1;
    }

    offset = (off_t)(block_num * UFS_BLOCK_SIZE);

    if (fseeko(disk, offset, SEEK_SET) != 0)
    {
        return -1;
    }

    if (fread(buf, 1, UFS_BLOCK_SIZE, disk) != UFS_BLOCK_SIZE)
    {
        errno = EIO;
        return -1;
    }

    return 0;
}

static int disk_write_block(uint64_t block_num, const void *buf)
{
    off_t offset;

    if (disk == NULL)
    {
        errno = EBADF;
        return -1;
    }

    if (sb.total_blocks != 0 && block_num >= sb.total_blocks)
    {
        errno = EINVAL;
        return -1;
    }

    offset = (off_t)(block_num * UFS_BLOCK_SIZE);

    if (fseeko(disk, offset, SEEK_SET) != 0)
    {
        return -1;
    }

    if (fwrite(buf, 1, UFS_BLOCK_SIZE, disk) != UFS_BLOCK_SIZE)
    {
        errno = EIO;
        return -1;
    }

    if (fflush(disk) != 0)
    {
        return -1;
    }

    return 0;
}

static int flush_inode_bitmap(void)
{
    uint32_t i;

    for (i = 0; i < sb.inode_bitmap_blocks; ++i)
    {
        uint64_t block_num = sb.inode_bitmap_start + i;
        uint8_t *buffer = inode_bitmap + (size_t)i * UFS_BLOCK_SIZE;

        if (disk_write_block(block_num, buffer) != 0)
        {
            return -1;
        }
    }

    return 0;
}

static int flush_block_bitmap(void)
{
    uint32_t i;

    for (i = 0; i < sb.block_bitmap_blocks; ++i)
    {
        uint64_t block_num = sb.block_bitmap_start + i;
        uint8_t *buffer = block_bitmap + (size_t)i * UFS_BLOCK_SIZE;

        if (disk_write_block(block_num, buffer) != 0)
        {
            return -1;
        }
    }

    return 0;
}

#define UFS_INODES_PER_BLOCK \
    (UFS_BLOCK_SIZE / sizeof(struct ufs_inode))

static int read_inode(uint32_t inum, struct ufs_inode *out)
{
    uint64_t block;
    uint32_t offset;
    uint8_t buf[UFS_BLOCK_SIZE];

    if (inum >= sb.total_inodes || out == NULL)
    {
        errno = EINVAL;
        return -1;
    }

    block = sb.inode_table_start +
            inum / UFS_INODES_PER_BLOCK;

    offset = (inum % UFS_INODES_PER_BLOCK) *
             sizeof(struct ufs_inode);

    if (disk_read_block(block, buf) != 0)
    {
        return -1;
    }

    memcpy(out, buf + offset, sizeof(struct ufs_inode));

    return 0;
}

static int write_inode(uint32_t inum,
                       const struct ufs_inode *in)
{
    uint64_t block;
    uint32_t offset;
    uint8_t buf[UFS_BLOCK_SIZE];

    if (inum >= sb.total_inodes || in == NULL)
    {
        errno = EINVAL;
        return -1;
    }

    block = sb.inode_table_start +
            inum / UFS_INODES_PER_BLOCK;

    offset = (inum % UFS_INODES_PER_BLOCK) *
             sizeof(struct ufs_inode);

    if (disk_read_block(block, buf) != 0)
    {
        return -1;
    }

    memcpy(buf + offset, in, sizeof(struct ufs_inode));

    return disk_write_block(block, buf);
}

static int64_t alloc_inode(void)
{
    int64_t inum;

    inum = bitmap_find_free(inode_bitmap, sb.total_inodes);

    if (inum < 0)
    {
        errno = ENOSPC;
        return -1;
    }

    bitmap_set(inode_bitmap, (uint64_t)inum);

    if (flush_inode_bitmap() != 0)
    {
        bitmap_clear(inode_bitmap, (uint64_t)inum);
        return -1;
    }

    return inum;
}

static int free_inode(uint32_t inum)
{
    if (inum >= sb.total_inodes)
    {
        errno = EINVAL;
        return -1;
    }

    bitmap_clear(inode_bitmap, inum);

    if (flush_inode_bitmap() != 0)
    {
        bitmap_set(inode_bitmap, inum);
        return -1;
    }

    return 0;
}

static int64_t alloc_block(void)
{
    uint64_t i;
    uint8_t zero[UFS_BLOCK_SIZE] = {0};

    /*
     * لا نحجز من أول الديسك؛ الـ metadata قبل data_start محجوزة.
     */
    for (i = sb.data_start; i < sb.total_blocks; ++i)
    {
        if (!bitmap_test(block_bitmap, i))
        {
            bitmap_set(block_bitmap, i);

            if (flush_block_bitmap() != 0)
            {
                bitmap_clear(block_bitmap, i);
                return -1;
            }

            if (disk_write_block(i, zero) != 0)
            {
                bitmap_clear(block_bitmap, i);
                (void)flush_block_bitmap();
                return -1;
            }

            return (int64_t)i;
        }
    }

    errno = ENOSPC;
    return -1;
}

static int free_block(uint64_t block_num)
{
    if (block_num < sb.data_start ||
        block_num >= sb.total_blocks)
    {
        errno = EINVAL;
        return -1;
    }

    bitmap_clear(block_bitmap, block_num);

    if (flush_block_bitmap() != 0)
    {
        bitmap_set(block_bitmap, block_num);
        return -1;
    }

    return 0;
}

/*
 * بدل dir_find_entry وufs_disk_dirent:
 * ندور داخل children[44]، ثم نقرأ child inode ونقارن اسمه.
 *
 * return:
 *   1  found
 *   0  not found
 *  -1  error
 */
/*
 * Directory في تصميمنا:
 *
 * directory.children[44]
 * directory.child_count
 *
 * الاسم موجود داخل child inode نفسه.
 */

#define UFS_POINTERS_PER_BLOCK \
    (UFS_BLOCK_SIZE / sizeof(uint32_t))

static uint64_t pointer_capacity(uint32_t level)
{
    uint64_t result = 1;

    while (level > 0)
    {
        result *= UFS_POINTERS_PER_BLOCK;
        level--;
    }

    return result;
}

/*
 * level = 1  -> pointer block يشاور على data blocks
 * level = 2  -> pointer block يشاور على pointer blocks
 * level = 3  -> triple indirect
 *
 * allocate = 0:
 *   للقراءة فقط. لو لا يوجد block يرجع out_block = 0.
 *
 * allocate = 1:
 *   للكتابة. يحجز pointer/data blocks عند الحاجة.
 */
static int pointer_tree_get(uint32_t *root_block,
                            uint32_t level,
                            uint64_t index,
                            int allocate,
                            uint32_t *out_block)
{
    uint32_t pointers[UFS_POINTERS_PER_BLOCK];
    uint64_t span;
    uint32_t slot;
    int64_t new_block;

    if (root_block == NULL || out_block == NULL || level == 0)
    {
        errno = EINVAL;
        return -1;
    }

    /*
     * لو لا يوجد أصلًا indirect block.
     */
    if (*root_block == 0)
    {
        if (!allocate)
        {
            *out_block = 0;
            return 0;
        }

        new_block = alloc_block();

        if (new_block < 0)
        {
            return -1;
        }

        *root_block = (uint32_t)new_block;
    }

    if (disk_read_block(*root_block, pointers) != 0)
    {
        return -1;
    }

    /*
     * Single indirect:
     * pointers[index] هو data block نفسه.
     */
    if (level == 1)
    {
        if (index >= UFS_POINTERS_PER_BLOCK)
        {
            errno = EFBIG;
            return -1;
        }

        slot = (uint32_t)index;

        if (pointers[slot] == 0 && allocate)
        {
            new_block = alloc_block();

            if (new_block < 0)
            {
                return -1;
            }

            pointers[slot] = (uint32_t)new_block;

            if (disk_write_block(*root_block, pointers) != 0)
            {
                return -1;
            }
        }

        *out_block = pointers[slot];
        return 0;
    }

    /*
     * Double/triple indirect:
     * نحدد أي pointer نأخذه في المستوى الحالي،
     * ثم ننزل مستوى أقل.
     */
    span = pointer_capacity(level - 1);
    slot = (uint32_t)(index / span);

    if (slot >= UFS_POINTERS_PER_BLOCK)
    {
        errno = EFBIG;
        return -1;
    }

    if (pointer_tree_get(&pointers[slot],
                         level - 1,
                         index % span,
                         allocate,
                         out_block) != 0)
    {
        return -1;
    }

    /*
     * pointers[slot] قد يكون اتغير لو حجزنا block جديد،
     * لذلك نكتب block الحالي مرة أخرى.
     */
    return disk_write_block(*root_block, pointers);
}

/*
 * يحول logical block number داخل الملف
 * إلى physical block number داخل disk.img.
 *
 * logical_block = 0  -> أول block في الملف
 * logical_block = 1  -> ثاني block
 * ...
 *
 * allocate = 0 للقراءة
 * allocate = 1 للكتابة
 *
 * ملاحظة:
 * الدالة تعدل inode في الـ RAM عند حجز indirect blocks.
 * بعد ufs_write لازم تعمل write_inode().
 */
static int file_get_data_block(struct ufs_inode *inode,
                               uint64_t logical_block,
                               int allocate,
                               uint32_t *out_block)
{
    uint64_t index;
    int64_t new_block;

    if (inode == NULL || out_block == NULL)
    {
        errno = EINVAL;
        return -1;
    }

    if (inode->type != UFS_TYPE_FILE)
    {
        errno = EISDIR;
        return -1;
    }

    /*
     * أول 10 blocks: direct blocks.
     */
    if (logical_block < 10)
    {
        if (inode->data.file.direct_blocks[logical_block] == 0 &&
            allocate)
        {
            new_block = alloc_block();

            if (new_block < 0)
            {
                return -1;
            }

            inode->data.file.direct_blocks[logical_block] =
                (uint32_t)new_block;
        }

        *out_block =
            inode->data.file.direct_blocks[logical_block];

        return 0;
    }

    /*
     * Single indirect:
     * من logical block 10 إلى 137.
     */
    index = logical_block - 10;

    if (index < UFS_POINTERS_PER_BLOCK)
    {
        return pointer_tree_get(
            &inode->data.file.indirect_block,
            1,
            index,
            allocate,
            out_block
        );
    }

    /*
     * Double indirect:
     * بعد الـ single indirect.
     */
    index -= UFS_POINTERS_PER_BLOCK;

    if (index < pointer_capacity(2))
    {
        return pointer_tree_get(
            &inode->data.file.double_indirect_block,
            2,
            index,
            allocate,
            out_block
        );
    }

    /*
     * Triple indirect:
     * بعد الـ double indirect.
     */
    index -= pointer_capacity(2);

    if (index < pointer_capacity(3))
    {
        return pointer_tree_get(
            &inode->data.file.triple_indirect_block,
            3,
            index,
            allocate,
            out_block
        );
    }

    errno = EFBIG;
    return -1;
}

/*
 * يفرّغ (يمسح) الـ pointer entry اللي بتشاور على logical_block،
 * من غير ما يحرر الـ pointer block نفسه.
 *
 * تستخدم عند تصغير الملف (truncate) عشان نصفّر الإشارة بعد
 * ما نكون حررنا الـ data block الفعلي بـ free_block().
 */
static int pointer_tree_clear(uint32_t *root_block,
                              uint32_t level,
                              uint64_t index)
{
    uint32_t pointers[UFS_POINTERS_PER_BLOCK];
    uint64_t span;
    uint32_t slot;

    if (root_block == NULL || level == 0)
    {
        errno = EINVAL;
        return -1;
    }

    if (*root_block == 0)
    {
        /* لا يوجد شيء لمسحه. */
        return 0;
    }

    if (disk_read_block(*root_block, pointers) != 0)
    {
        return -1;
    }

    if (level == 1)
    {
        slot = (uint32_t)index;

        if (slot >= UFS_POINTERS_PER_BLOCK)
        {
            errno = EFBIG;
            return -1;
        }

        pointers[slot] = 0;
        return disk_write_block(*root_block, pointers);
    }

    span = pointer_capacity(level - 1);
    slot = (uint32_t)(index / span);

    if (slot >= UFS_POINTERS_PER_BLOCK)
    {
        errno = EFBIG;
        return -1;
    }

    if (pointers[slot] == 0)
    {
        return 0;
    }

    if (pointer_tree_clear(&pointers[slot], level - 1, index % span) != 0)
    {
        return -1;
    }

    return disk_write_block(*root_block, pointers);
}

/*
 * يمسح الـ pointer entry الخاصة بـ logical_block داخل inode
 * (direct block أو داخل شجرة الـ indirect).
 */
static int file_clear_data_block(struct ufs_inode *inode,
                                 uint64_t logical_block)
{
    uint64_t index;

    if (inode == NULL || inode->type != UFS_TYPE_FILE)
    {
        errno = EINVAL;
        return -1;
    }

    if (logical_block < 10)
    {
        inode->data.file.direct_blocks[logical_block] = 0;
        return 0;
    }

    index = logical_block - 10;

    if (index < UFS_POINTERS_PER_BLOCK)
    {
        return pointer_tree_clear(&inode->data.file.indirect_block,
                                  1, index);
    }

    index -= UFS_POINTERS_PER_BLOCK;

    if (index < pointer_capacity(2))
    {
        return pointer_tree_clear(&inode->data.file.double_indirect_block,
                                  2, index);
    }

    index -= pointer_capacity(2);

    return pointer_tree_clear(&inode->data.file.triple_indirect_block,
                              3, index);
}

/*
 * يحرر شجرة pointer/data blocks كلها بدءًا من root_block.
 *
 * level = 1 -> pointers[i] كلها data blocks، تتحرر مباشرة.
 * level > 1 -> pointers[i] كلها pointer blocks، بننزل مستوى أقل
 *              بشكل recursive قبل ما نحررها.
 *
 * في النهاية يحرر root_block نفسه ويصفّره.
 */
static int pointer_tree_free(uint32_t *root_block, uint32_t level)
{
    uint32_t pointers[UFS_POINTERS_PER_BLOCK];
    uint32_t i;

    if (root_block == NULL || level == 0)
    {
        errno = EINVAL;
        return -1;
    }

    if (*root_block == 0)
    {
        return 0;
    }

    if (disk_read_block(*root_block, pointers) != 0)
    {
        return -1;
    }

    for (i = 0; i < UFS_POINTERS_PER_BLOCK; ++i)
    {
        if (pointers[i] == 0)
        {
            continue;
        }

        if (level == 1)
        {
            if (free_block(pointers[i]) != 0)
            {
                return -1;
            }
        }
        else
        {
            if (pointer_tree_free(&pointers[i], level - 1) != 0)
            {
                return -1;
            }
        }
    }

    if (free_block(*root_block) != 0)
    {
        return -1;
    }

    *root_block = 0;
    return 0;
}

/*
 * يحرر كل الـ data blocks الخاصة بملف (direct + indirect
 * levels الثلاثة)، ويصفّر حجمه. تُستخدم في ufs_unlink وفي
 * truncate لصفر.
 */
static int file_free_all_blocks(struct ufs_inode *inode)
{
    int i;

    if (inode == NULL || inode->type != UFS_TYPE_FILE)
    {
        errno = EINVAL;
        return -1;
    }

    for (i = 0; i < 10; ++i)
    {
        if (inode->data.file.direct_blocks[i] != 0)
        {
            if (free_block(inode->data.file.direct_blocks[i]) != 0)
            {
                return -1;
            }

            inode->data.file.direct_blocks[i] = 0;
        }
    }

    if (pointer_tree_free(&inode->data.file.indirect_block, 1) != 0)
    {
        return -1;
    }

    if (pointer_tree_free(&inode->data.file.double_indirect_block, 2) != 0)
    {
        return -1;
    }

    if (pointer_tree_free(&inode->data.file.triple_indirect_block, 3) != 0)
    {
        return -1;
    }

    inode->data.file.block_count = 0;
    inode->size = 0;

    return 0;
}

/*
 * يعدّل عدد الـ blocks المحجوزة لملف بحيث يطابق new_size.
 *
 * تصغير (new_size < الحجم الحالي):
 *   نحرر كل الـ data blocks اللي بقت زيادة عن new_size،
 *   من آخر block للأول، ونصفّر الإشارة ليها في الـ inode.
 *   لو الحجم الجديد صفر، نحرر أيضًا شجرة الـ indirect بالكامل.
 *
 * تكبير (new_size >= الحجم الحالي):
 *   لا نحجز blocks فورًا (sparse) - الحجز الفعلي بيحصل
 *   عند أول ufs_write على تلك المنطقة.
 */
static int file_truncate_blocks(struct ufs_inode *inode, uint64_t new_size)
{
    uint64_t old_blocks;
    uint64_t new_blocks;
    uint64_t logical;

    if (inode == NULL || inode->type != UFS_TYPE_FILE)
    {
        errno = EINVAL;
        return -1;
    }

    if (new_size == 0)
    {
        return file_free_all_blocks(inode);
    }

    old_blocks = (inode->size + UFS_BLOCK_SIZE - 1) / UFS_BLOCK_SIZE;
    new_blocks = (new_size + UFS_BLOCK_SIZE - 1) / UFS_BLOCK_SIZE;

    if (new_blocks < old_blocks)
    {
        for (logical = old_blocks; logical > new_blocks; --logical)
        {
            uint32_t phys_block = 0;

            if (file_get_data_block(inode, logical - 1, 0,
                                    &phys_block) != 0)
            {
                return -1;
            }

            if (phys_block != 0)
            {
                if (free_block(phys_block) != 0)
                {
                    return -1;
                }

                if (file_clear_data_block(inode, logical - 1) != 0)
                {
                    return -1;
                }
            }
        }
    }

    inode->size = new_size;
    return 0;
}









static int dir_find_child(const struct ufs_inode *dir,
                          const char *name,
                          uint32_t *out_inode)
{
    uint32_t i;
    struct ufs_inode child;

    if (dir == NULL || name == NULL)
    {
        errno = EINVAL;
        return -1;
    }

    if (dir->type != UFS_TYPE_DIR)
    {
        errno = ENOTDIR;
        return -1;
    }

    for (i = 0; i < dir->data.directory.child_count; ++i)
    {
        uint32_t child_inum =
            dir->data.directory.children[i];

        if (read_inode(child_inum, &child) != 0)
        {
            return -1;
        }

        if (child.used && strcmp(child.name, name) == 0)
        {
            if (out_inode != NULL)
            {
                *out_inode = child_inum;
            }

            return 1; /* Found */
        }
    }

    return 0; /* Not found */
}

static int dir_add_child(uint32_t parent_inum,
                         struct ufs_inode *parent,
                         uint32_t child_inum)
{
    uint32_t count;

    if (parent == NULL)
    {
        errno = EINVAL;
        return -1;
    }

    if (parent->type != UFS_TYPE_DIR)
    {
        errno = ENOTDIR;
        return -1;
    }

    count = parent->data.directory.child_count;

    if (count >= 44)
    {
        errno = ENOSPC;
        return -1;
    }

    parent->data.directory.children[count] = child_inum;
    parent->data.directory.child_count = count + 1;
    parent->modified_at = (int64_t)time(NULL);

    return write_inode(parent_inum, parent);
}

static int dir_remove_child(uint32_t parent_inum,
                            struct ufs_inode *parent,
                            uint32_t child_inum)
{
    uint32_t i;
    uint32_t count;

    if (parent == NULL)
    {
        errno = EINVAL;
        return -1;
    }

    if (parent->type != UFS_TYPE_DIR)
    {
        errno = ENOTDIR;
        return -1;
    }

    count = parent->data.directory.child_count;

    for (i = 0; i < count; ++i)
    {
        if (parent->data.directory.children[i] == child_inum)
        {
            if (i + 1 < count)
            {
                memmove(
                    &parent->data.directory.children[i],
                    &parent->data.directory.children[i + 1],
                    (count - i - 1) * sizeof(uint32_t)
                );
            }

            parent->data.directory.children[count - 1] = 0;
            parent->data.directory.child_count = count - 1;
            parent->modified_at = (int64_t)time(NULL);

            return write_inode(parent_inum, parent);
        }
    }

    errno = ENOENT;
    return -1;
}

static int dir_is_empty(const struct ufs_inode *dir)
{
    if (dir == NULL || dir->type != UFS_TYPE_DIR)
    {
        return 0;
    }

    return dir->data.directory.child_count == 0;
}

static int resolve_path(const char *path, uint32_t *out_inode)
{
    uint32_t current_inode;
    uint32_t next_inode;
    char copy[UFS_MAX_PATH + 1];
    char *token;
    char *save_ptr = NULL;
    struct ufs_inode current;
    int found;

    if (path == NULL || out_inode == NULL || path[0] != '/')
    {
        errno = EINVAL;
        return -1;
    }

    if (strlen(path) > UFS_MAX_PATH)
    {
        errno = ENAMETOOLONG;
        return -1;
    }

    current_inode = sb.root_inode;

    if (strcmp(path, "/") == 0)
    {
        *out_inode = current_inode;
        return 0;
    }

    strcpy(copy, path);

    token = strtok_r(copy, "/", &save_ptr);

    while (token != NULL)
    {
        if (read_inode(current_inode, &current) != 0)
        {
            return -1;
        }

        if (current.type != UFS_TYPE_DIR)
        {
            errno = ENOTDIR;
            return -1;
        }

        found = dir_find_child(&current, token, &next_inode);

        if (found < 0)
        {
            return -1;
        }

        if (found == 0)
        {
            errno = ENOENT;
            return -1;
        }

        current_inode = next_inode;
        token = strtok_r(NULL, "/", &save_ptr);
    }

    *out_inode = current_inode;
    return 0;
}

static int resolve_parent(const char *path,
                          uint32_t *parent_inode,
                          char *name_out)
{
    char copy[UFS_MAX_PATH + 1];
    char *slash;
    size_t length;

    if (path == NULL || parent_inode == NULL ||
        name_out == NULL || path[0] != '/')
    {
        errno = EINVAL;
        return -1;
    }

    length = strlen(path);

    if (length == 0 || length > UFS_MAX_PATH)
    {
        errno = ENAMETOOLONG;
        return -1;
    }

    if (strcmp(path, "/") == 0)
    {
        errno = EINVAL;
        return -1;
    }

    strcpy(copy, path);

    /*
     * يسمح بـ /docs/ كمجلد docs،
     * لكن لا يسمح بأن ينتهي الاسم فارغًا بعد إزالة slash.
     */
    length = strlen(copy);

    if (length > 1 && copy[length - 1] == '/')
    {
        copy[length - 1] = '\0';
    }

    slash = strrchr(copy, '/');

    if (slash == NULL)
    {
        errno = EINVAL;
        return -1;
    }

    strcpy(name_out, slash + 1);

    if (name_out[0] == '\0' ||
        strlen(name_out) > UFS_MAX_NAME)
    {
        errno = ENAMETOOLONG;
        return -1;
    }

    if (slash == copy)
    {
        *parent_inode = sb.root_inode;
        return 0;
    }

    *slash = '\0';

    return resolve_path(copy, parent_inode);
}

/*
 * اختياري: ليس موجودًا في userfs.h الحالي.
 * احتفظ به فقط إذا كانت دوال أخرى في مشروعكم تستدعيه.
 */
static int64_t ufs_get_file_size(int fd)
{
    uint32_t inode_number;
    struct ufs_inode inode;

    if (fd < 0 || fd >= UFS_MAX_OPEN_FILES ||
        !open_files[fd].used)
    {
        errno = EBADF;
        return -1;
    }

    inode_number = (uint32_t)open_files[fd].inode_number;

    if (read_inode(inode_number, &inode) != 0)
    {
        return -1;
    }

    return (int64_t)inode.size;
}

int ufs_format(const char *image_path, size_t image_size)
{
    FILE *f = NULL;
    struct ufs_superblock new_sb;
    struct ufs_inode root;

    uint64_t total_blocks;
    uint64_t i;

    uint8_t zero[UFS_BLOCK_SIZE] = {0};
    uint8_t sb_block[UFS_BLOCK_SIZE] = {0};
    uint8_t inode_block[UFS_BLOCK_SIZE] = {0};

    uint8_t *ibmp = NULL;
    uint8_t *bbmp = NULL;

    size_t ibmp_bytes;
    size_t bbmp_bytes;

    if (image_path == NULL ||
        image_size < UFS_BLOCK_SIZE * 32 ||
        image_size % UFS_BLOCK_SIZE != 0)
    {
        errno = EINVAL;
        return -1;
    }

    total_blocks = image_size / UFS_BLOCK_SIZE;

    if (total_blocks / 4 > UINT32_MAX)
    {
        errno = EFBIG;
        return -1;
    }

    f = fopen(image_path, "wb+");

    if (f == NULL)
    {
        return -1;
    }

    /*
     * نجهز حجم الـ disk image ونصفّره block block.
     */
    for (i = 0; i < total_blocks; ++i)
    {
        if (fwrite(zero, 1, UFS_BLOCK_SIZE, f) != UFS_BLOCK_SIZE)
        {
            errno = EIO;
            goto fail;
        }
    }

    memset(&new_sb, 0, sizeof(new_sb));

    new_sb.magic = UFS_MAGIC;
    new_sb.version = UFS_VERSION;
    new_sb.block_size = UFS_BLOCK_SIZE;

    new_sb.total_blocks = total_blocks;

    /*
     * كل inode حجمها 256 bytes.
     * نختار عدد inodes = ربع عدد الـ blocks، بحد أدنى 16.
     */
    new_sb.total_inodes = (uint32_t)(total_blocks / 4);

    if (new_sb.total_inodes < 16)
    {
        new_sb.total_inodes = 16;
    }

    /*
     * Block 0 = superblock
     */
    new_sb.inode_bitmap_start = 1;

    new_sb.inode_bitmap_blocks =
        (uint32_t)bitmap_blocks_needed(new_sb.total_inodes);

    new_sb.block_bitmap_start =
        new_sb.inode_bitmap_start +
        new_sb.inode_bitmap_blocks;

    new_sb.block_bitmap_blocks =
        (uint32_t)bitmap_blocks_needed(new_sb.total_blocks);

    new_sb.inode_table_start =
        new_sb.block_bitmap_start +
        new_sb.block_bitmap_blocks;

    new_sb.inode_table_blocks =
        (uint32_t)(
            ((uint64_t)new_sb.total_inodes *
             sizeof(struct ufs_inode) +
             UFS_BLOCK_SIZE - 1) /
            UFS_BLOCK_SIZE
        );

    new_sb.data_start =
        new_sb.inode_table_start +
        new_sb.inode_table_blocks;

    if (new_sb.data_start >= new_sb.total_blocks)
    {
        errno = ENOSPC;
        goto fail;
    }

    new_sb.data_blocks =
        new_sb.total_blocks - new_sb.data_start;

    new_sb.root_inode = 0;

    /*
     * Write superblock in block 0.
     */
    memcpy(sb_block, &new_sb, sizeof(new_sb));

    if (fseeko(f, 0, SEEK_SET) != 0 ||
        fwrite(sb_block, 1, UFS_BLOCK_SIZE, f) != UFS_BLOCK_SIZE)
    {
        errno = EIO;
        goto fail;
    }

    /*
     * Inode bitmap:
     * inode 0 محجوز للـ root directory.
     */
    ibmp_bytes =
        (size_t)new_sb.inode_bitmap_blocks *
        UFS_BLOCK_SIZE;

    ibmp = calloc(1, ibmp_bytes);

    if (ibmp == NULL)
    {
        errno = ENOMEM;
        goto fail;
    }

    bitmap_set(ibmp, 0);

    if (fseeko(f,
               (off_t)new_sb.inode_bitmap_start *
               UFS_BLOCK_SIZE,
               SEEK_SET) != 0 ||
        fwrite(ibmp, 1, ibmp_bytes, f) != ibmp_bytes)
    {
        errno = EIO;
        goto fail;
    }

    free(ibmp);
    ibmp = NULL;

    /*
     * Block bitmap:
     * كل blocks قبل data_start هي metadata، فلازم تتعلم used.
     */
    bbmp_bytes =
        (size_t)new_sb.block_bitmap_blocks *
        UFS_BLOCK_SIZE;

    bbmp = calloc(1, bbmp_bytes);

    if (bbmp == NULL)
    {
        errno = ENOMEM;
        goto fail;
    }

    for (i = 0; i < new_sb.data_start; ++i)
    {
        bitmap_set(bbmp, i);
    }

    if (fseeko(f,
               (off_t)new_sb.block_bitmap_start *
               UFS_BLOCK_SIZE,
               SEEK_SET) != 0 ||
        fwrite(bbmp, 1, bbmp_bytes, f) != bbmp_bytes)
    {
        errno = EIO;
        goto fail;
    }

    free(bbmp);
    bbmp = NULL;

    /*
     * inode 0 = root directory.
     *
     * الـ directory عندنا لا تستخدم direct blocks.
     * الأطفال يتخزنوا لاحقًا في:
     * root.data.directory.children[44]
     */
    memset(&root, 0, sizeof(root));

    root.used = 1;
    root.type = UFS_TYPE_DIR;

    root.name[0] = '\0';

    root.size = 0;
    root.parent_inode = -1;
    root.permissions = 0755;
    root.created_at = (int64_t)time(NULL);
    root.modified_at = root.created_at;

    root.data_blocks = 0;

    root.data.directory.child_count = 0;

    memcpy(inode_block, &root, sizeof(root));

    if (fseeko(f,
               (off_t)new_sb.inode_table_start *
               UFS_BLOCK_SIZE,
               SEEK_SET) != 0 ||
        fwrite(inode_block, 1, UFS_BLOCK_SIZE, f) !=
        UFS_BLOCK_SIZE)
    {
        errno = EIO;
        goto fail;
    }

    if (fflush(f) != 0)
    {
        goto fail;
    }

    fclose(f);
    return 0;

fail:
    free(ibmp);
    free(bbmp);

    if (f != NULL)
    {
        fclose(f);
    }

    unlink(image_path);
    return -1;
}

int ufs_mount(const char *image_path)
{
    size_t ibmp_bytes;
    size_t bbmp_bytes;
    off_t image_bytes;
    struct ufs_inode root;

    if (image_path == NULL)
    {
        errno = EINVAL;
        return -1;
    }

    if (disk != NULL)
    {
        errno = EBUSY;
        return -1;
    }

    disk = fopen(image_path, "rb+");

    if (disk == NULL)
    {
        return -1;
    }

    /*
     * نقرأ الـ superblock من block 0.
     */
    if (fseeko(disk, 0, SEEK_SET) != 0 ||
        fread(&sb, 1, sizeof(sb), disk) != sizeof(sb))
    {
        errno = EIO;
        goto fail;
    }

    /*
     * نتأكد إن الـ image فعلًا UserFS بالتصميم المتوقع.
     */
    if (sb.magic != UFS_MAGIC ||
        sb.version != UFS_VERSION ||
        sb.block_size != UFS_BLOCK_SIZE ||
        sb.total_blocks == 0 ||
        sb.total_inodes == 0 ||
        sb.root_inode >= sb.total_inodes ||
        sb.inode_bitmap_start != 1 ||
        sb.inode_bitmap_blocks == 0 ||
        sb.block_bitmap_blocks == 0 ||
        sb.inode_table_blocks == 0 ||
        sb.data_start >= sb.total_blocks ||
        sb.data_blocks != sb.total_blocks - sb.data_start)
    {
        errno = EINVAL;
        goto fail;
    }

    /*
     * نتأكد أن أماكن الـ metadata لا تخرج من الديسك.
     */
    if ((uint64_t)sb.inode_bitmap_start +
            sb.inode_bitmap_blocks > sb.total_blocks ||
        (uint64_t)sb.block_bitmap_start +
            sb.block_bitmap_blocks > sb.total_blocks ||
        (uint64_t)sb.inode_table_start +
            sb.inode_table_blocks > sb.total_blocks ||
        sb.inode_table_start >= sb.data_start)
    {
        errno = EINVAL;
        goto fail;
    }

    /*
     * نتأكد أن حجم الملف الحقيقي يكفي كل الـ blocks المعلن عنها.
     */
    if (fseeko(disk, 0, SEEK_END) != 0)
    {
        goto fail;
    }

    image_bytes = ftello(disk);

    if (image_bytes < 0 ||
        (uint64_t)image_bytes <
            sb.total_blocks * UFS_BLOCK_SIZE)
    {
        errno = EINVAL;
        goto fail;
    }

    /*
     * نحمّل inode bitmap إلى RAM.
     */
    ibmp_bytes =
        (size_t)sb.inode_bitmap_blocks * UFS_BLOCK_SIZE;

    inode_bitmap = malloc(ibmp_bytes);

    if (inode_bitmap == NULL)
    {
        errno = ENOMEM;
        goto fail;
    }

    if (fseeko(disk,
               (off_t)sb.inode_bitmap_start *
               UFS_BLOCK_SIZE,
               SEEK_SET) != 0 ||
        fread(inode_bitmap, 1, ibmp_bytes, disk) != ibmp_bytes)
    {
        errno = EIO;
        goto fail;
    }

    /*
     * نحمّل block bitmap إلى RAM.
     */
    bbmp_bytes =
        (size_t)sb.block_bitmap_blocks * UFS_BLOCK_SIZE;

    block_bitmap = malloc(bbmp_bytes);

    if (block_bitmap == NULL)
    {
        errno = ENOMEM;
        goto fail;
    }

    if (fseeko(disk,
               (off_t)sb.block_bitmap_start *
               UFS_BLOCK_SIZE,
               SEEK_SET) != 0 ||
        fread(block_bitmap, 1, bbmp_bytes, disk) != bbmp_bytes)
    {
        errno = EIO;
        goto fail;
    }

    /*
     * نتحقق أن الـ root inode موجودة وفعلًا directory.
     */
    if (read_inode(sb.root_inode, &root) != 0 ||
        !root.used ||
        root.type != UFS_TYPE_DIR)
    {
        errno = EINVAL;
        goto fail;
    }

    /*
     * open-files جدول Runtime فقط،
     * لذلك يبدأ فاضي في كل mount.
     */
    memset(open_files, 0, sizeof(open_files));

    return 0;

fail:
    free(inode_bitmap);
    free(block_bitmap);

    inode_bitmap = NULL;
    block_bitmap = NULL;

    fclose(disk);
    disk = NULL;

    return -1;
}

int ufs_unmount(void)
{
    if (disk == NULL)
    {
        errno = EINVAL;
        return -1;
    }

    /*
     * نحفظ أي تعديل في الـ allocation maps.
     */
    if (flush_inode_bitmap() != 0 ||
        flush_block_bitmap() != 0)
    {
        return -1;
    }

    if (fflush(disk) != 0)
    {
        return -1;
    }

    if (fclose(disk) != 0)
    {
        disk = NULL;
        return -1;
    }

    disk = NULL;

    free(inode_bitmap);
    free(block_bitmap);

    inode_bitmap = NULL;
    block_bitmap = NULL;

    memset(&sb, 0, sizeof(sb));

    /*
     * الـ table ده Runtime فقط.
     */
    memset(open_files, 0, sizeof(open_files));

    return 0;
}

int ufs_mkdir(const char *path)
{
    uint32_t parent_inum;
    uint32_t new_inum;

    char name[UFS_MAX_NAME + 1];

    struct ufs_inode parent;
    struct ufs_inode new_dir;

    int found;
    int64_t allocated_inum;

    if (disk == NULL)
    {
        errno = EINVAL;
        return -1;
    }

    if (resolve_parent(path, &parent_inum, name) != 0)
    {
        return -1;
    }

    if (read_inode(parent_inum, &parent) != 0)
    {
        return -1;
    }

    if (parent.type != UFS_TYPE_DIR)
    {
        errno = ENOTDIR;
        return -1;
    }

    /*
     * هل يوجد child بالاسم نفسه بالفعل؟
     */
    found = dir_find_child(&parent, name, NULL);

    if (found < 0)
    {
        return -1;
    }

    if (found == 1)
    {
        errno = EEXIST;
        return -1;
    }

    /*
     * نحجز inode جديدة للـ directory.
     */
    allocated_inum = alloc_inode();

    if (allocated_inum < 0)
    {
        return -1;
    }

    new_inum = (uint32_t)allocated_inum;

    /*
     * نجهز inode الخاصة بالـ directory الجديدة.
     */
    memset(&new_dir, 0, sizeof(new_dir));

    new_dir.used = 1;
    new_dir.type = UFS_TYPE_DIR;

    strncpy(new_dir.name, name, UFS_MAX_NAME);
    new_dir.name[UFS_MAX_NAME] = '\0';

    new_dir.size = 0;
    new_dir.parent_inode = (int32_t)parent_inum;
    new_dir.permissions = 0755;

    new_dir.created_at = (int64_t)time(NULL);
    new_dir.modified_at = new_dir.created_at;

    /*
     * الـ directory لا تستخدم data blocks عندنا.
     * الأطفال محفوظون في children[44].
     */
    new_dir.data_blocks = 0;
    new_dir.data.directory.child_count = 0;

    if (write_inode(new_inum, &new_dir) != 0)
    {
        (void)free_inode(new_inum);
        return -1;
    }

    /*
     * نربط الـ child الجديد بالـ parent directory.
     */
    if (dir_add_child(parent_inum, &parent, new_inum) != 0)
    {
        int saved_errno = errno;

        memset(&new_dir, 0, sizeof(new_dir));

        (void)write_inode(new_inum, &new_dir);
        (void)free_inode(new_inum);

        errno = saved_errno;
        return -1;
    }

    return 0;
}
int ufs_rmdir(const char *path)
{
    uint32_t parent_inum;
    uint32_t child_inum;

    char name[UFS_MAX_NAME + 1];

    struct ufs_inode parent;
    struct ufs_inode child;

    int found;

    if (disk == NULL)
    {
        errno = EINVAL;
        return -1;
    }

    if (resolve_parent(path, &parent_inum, name) != 0)
    {
        return -1;
    }

    if (read_inode(parent_inum, &parent) != 0)
    {
        return -1;
    }

    if (parent.type != UFS_TYPE_DIR)
    {
        errno = ENOTDIR;
        return -1;
    }

    found = dir_find_child(&parent, name, &child_inum);

    if (found < 0)
    {
        return -1;
    }

    if (found == 0)
    {
        errno = ENOENT;
        return -1;
    }

    if (read_inode(child_inum, &child) != 0)
    {
        return -1;
    }

    if (child.type != UFS_TYPE_DIR)
    {
        errno = ENOTDIR;
        return -1;
    }

    /*
     * لا يجوز حذف الـ root.
     */
    if (child_inum == sb.root_inode)
    {
        errno = EBUSY;
        return -1;
    }

    if (!dir_is_empty(&child))
    {
        errno = ENOTEMPTY;
        return -1;
    }

    if (dir_remove_child(parent_inum, &parent, child_inum) != 0)
    {
        return -1;
    }

    memset(&child, 0, sizeof(child));

    if (write_inode(child_inum, &child) != 0)
    {
        return -1;
    }

    return free_inode(child_inum);
}

int ufs_listdir(const char *path, struct ufs_dirent *entries, size_t max_entries)
{
    uint32_t dir_inum;
    uint32_t i;
    size_t count;

    struct ufs_inode dir;
    struct ufs_inode child;

    if (disk == NULL)
    {
        errno = EINVAL;
        return -1;
    }

    if (path == NULL || (entries == NULL && max_entries > 0))
    {
        errno = EINVAL;
        return -1;
    }

    if (resolve_path(path, &dir_inum) != 0)
    {
        return -1;
    }

    if (read_inode(dir_inum, &dir) != 0)
    {
        return -1;
    }

    if (dir.type != UFS_TYPE_DIR)
    {
        errno = ENOTDIR;
        return -1;
    }

    count = 0;

    for (i = 0; i < dir.data.directory.child_count; ++i)
    {
        uint32_t child_inum = dir.data.directory.children[i];

        if (read_inode(child_inum, &child) != 0)
        {
            return -1;
        }

        if (!child.used)
        {
            continue;
        }

        /*
         * لو الـ buffer الممرر أصغر من عدد الأطفال،
         * نكمل نعد بس من غير ما نكتب برة حدود الـ array.
         */
        if (count < max_entries)
        {
            strncpy(entries[count].name, child.name, UFS_MAX_NAME);
            entries[count].name[UFS_MAX_NAME] = '\0';

            entries[count].type = (int)child.type;
            entries[count].size = (size_t)child.size;
        }

        count++;
    }

    return (int)count;
}

int ufs_create(const char *path)
{
    uint32_t parent_inum;
    uint32_t new_inum;

    char name[UFS_MAX_NAME + 1];

    struct ufs_inode parent;
    struct ufs_inode new_file;

    int found;
    int64_t allocated_inum;

    if (disk == NULL)
    {
        errno = EINVAL;
        return -1;
    }

    if (resolve_parent(path, &parent_inum, name) != 0)
    {
        return -1;
    }

    if (read_inode(parent_inum, &parent) != 0)
    {
        return -1;
    }

    if (parent.type != UFS_TYPE_DIR)
    {
        errno = ENOTDIR;
        return -1;
    }

    /*
     * هل يوجد child بالاسم نفسه بالفعل؟
     */
    found = dir_find_child(&parent, name, NULL);

    if (found < 0)
    {
        return -1;
    }

    if (found == 1)
    {
        errno = EEXIST;
        return -1;
    }

    allocated_inum = alloc_inode();

    if (allocated_inum < 0)
    {
        return -1;
    }

    new_inum = (uint32_t)allocated_inum;

    memset(&new_file, 0, sizeof(new_file));

    new_file.used = 1;
    new_file.type = UFS_TYPE_FILE;

    strncpy(new_file.name, name, UFS_MAX_NAME);
    new_file.name[UFS_MAX_NAME] = '\0';

    new_file.size = 0;
    new_file.parent_inode = (int32_t)parent_inum;
    new_file.permissions = 0644;

    new_file.created_at = (int64_t)time(NULL);
    new_file.modified_at = new_file.created_at;

    /*
     * ملف جديد وفاضي: لا direct/indirect blocks لسه.
     */
    new_file.data_blocks = 0;
    new_file.data.file.indirect_block = 0;
    new_file.data.file.double_indirect_block = 0;
    new_file.data.file.triple_indirect_block = 0;
    new_file.data.file.block_count = 0;

    if (write_inode(new_inum, &new_file) != 0)
    {
        (void)free_inode(new_inum);
        return -1;
    }

    if (dir_add_child(parent_inum, &parent, new_inum) != 0)
    {
        int saved_errno = errno;

        memset(&new_file, 0, sizeof(new_file));

        (void)write_inode(new_inum, &new_file);
        (void)free_inode(new_inum);

        errno = saved_errno;
        return -1;
    }

    return 0;
}

int ufs_unlink(const char *path)
{
    uint32_t parent_inum;
    uint32_t child_inum;

    char name[UFS_MAX_NAME + 1];

    struct ufs_inode parent;
    struct ufs_inode child;

    int found;
    int i;

    if (disk == NULL)
    {
        errno = EINVAL;
        return -1;
    }

    if (resolve_parent(path, &parent_inum, name) != 0)
    {
        return -1;
    }

    if (read_inode(parent_inum, &parent) != 0)
    {
        return -1;
    }

    if (parent.type != UFS_TYPE_DIR)
    {
        errno = ENOTDIR;
        return -1;
    }

    found = dir_find_child(&parent, name, &child_inum);

    if (found < 0)
    {
        return -1;
    }

    if (found == 0)
    {
        errno = ENOENT;
        return -1;
    }

    if (read_inode(child_inum, &child) != 0)
    {
        return -1;
    }

    if (child.type != UFS_TYPE_FILE)
    {
        errno = EISDIR;
        return -1;
    }

    /*
     * أي fd مفتوح على الملف ده لازم يتقفل،
     * عشان ما يفضلش يشاور على inode هيتم تحريرها.
     */
    for (i = 0; i < UFS_MAX_OPEN_FILES; ++i)
    {
        if (open_files[i].used &&
            (uint32_t)open_files[i].inode_number == child_inum)
        {
            open_files[i].used = 0;
        }
    }

    if (dir_remove_child(parent_inum, &parent, child_inum) != 0)
    {
        return -1;
    }

    if (file_free_all_blocks(&child) != 0)
    {
        return -1;
    }

    memset(&child, 0, sizeof(child));

    if (write_inode(child_inum, &child) != 0)
    {
        return -1;
    }

    return free_inode(child_inum);
}

int ufs_open(const char *path, int flags)
{
    uint32_t inum;
    struct ufs_inode inode;
    int fd;
    int access_mode;

    if (disk == NULL)
    {
        errno = EINVAL;
        return -1;
    }

    if (path == NULL)
    {
        errno = EINVAL;
        return -1;
    }

    /*
     * UFS_O_APPEND هو bit إضافي فوق وضع الوصول الأساسي،
     * فبنفصله عشان نتأكد إن الوضع الأساسي صحيح.
     */
    access_mode = flags & UFS_O_RDWR;

    if (access_mode != UFS_O_RDONLY &&
        access_mode != UFS_O_WRONLY &&
        access_mode != UFS_O_RDWR)
    {
        errno = EINVAL;
        return -1;
    }

    if (resolve_path(path, &inum) != 0)
    {
        return -1;
    }

    if (read_inode(inum, &inode) != 0)
    {
        return -1;
    }

    if (inode.type != UFS_TYPE_FILE)
    {
        errno = EISDIR;
        return -1;
    }

    for (fd = 0; fd < UFS_MAX_OPEN_FILES; ++fd)
    {
        if (!open_files[fd].used)
        {
            break;
        }
    }

    if (fd == UFS_MAX_OPEN_FILES)
    {
        errno = EMFILE;
        return -1;
    }

    open_files[fd].used = 1;
    open_files[fd].inode_number = (int)inum;
    open_files[fd].flags = flags;

    /*
     * O_APPEND: كل كتابة تبدأ من نهاية الملف.
     * نبدأ الـ offset هناك من الفتح.
     */
    open_files[fd].offset =
        (flags & UFS_O_APPEND) ? (off_t)inode.size : 0;

    return fd;
}

int ufs_close(int fd)
{
    if (fd < 0 || fd >= UFS_MAX_OPEN_FILES || !open_files[fd].used)
    {
        errno = EBADF;
        return -1;
    }

    memset(&open_files[fd], 0, sizeof(open_files[fd]));

    return 0;
}

ssize_t ufs_read(int fd, void *buf, size_t count)
{
    struct ufs_open_file *of;
    struct ufs_inode inode;
    uint8_t block_buf[UFS_BLOCK_SIZE];
    uint8_t *out = (uint8_t *)buf;
    size_t total_read;

    if (disk == NULL)
    {
        errno = EINVAL;
        return -1;
    }

    if (fd < 0 || fd >= UFS_MAX_OPEN_FILES || !open_files[fd].used)
    {
        errno = EBADF;
        return -1;
    }

    if (buf == NULL && count > 0)
    {
        errno = EINVAL;
        return -1;
    }

    of = &open_files[fd];

    /*
     * ملف اتفتح WRONLY فقط لا يجوز نقرأ منه.
     */
    if ((of->flags & UFS_O_RDWR) == UFS_O_WRONLY)
    {
        errno = EBADF;
        return -1;
    }

    if (read_inode((uint32_t)of->inode_number, &inode) != 0)
    {
        return -1;
    }

    total_read = 0;

    while (total_read < count)
    {
        uint64_t logical_block;
        uint32_t phys_block;
        size_t block_offset;
        size_t to_copy;

        if ((uint64_t)of->offset >= inode.size)
        {
            break; /* وصلنا لنهاية الملف. */
        }

        logical_block = (uint64_t)of->offset / UFS_BLOCK_SIZE;
        block_offset = (size_t)((uint64_t)of->offset % UFS_BLOCK_SIZE);

        if (file_get_data_block(&inode, logical_block, 0,
                                &phys_block) != 0)
        {
            return (total_read > 0) ? (ssize_t)total_read : -1;
        }

        to_copy = UFS_BLOCK_SIZE - block_offset;

        if (to_copy > count - total_read)
        {
            to_copy = count - total_read;
        }

        if ((uint64_t)of->offset + to_copy > inode.size)
        {
            to_copy = (size_t)(inode.size - (uint64_t)of->offset);
        }

        if (to_copy == 0)
        {
            break;
        }

        if (phys_block == 0)
        {
            /* منطقة فارغة (sparse hole): نرجع أصفار. */
            memset(out + total_read, 0, to_copy);
        }
        else
        {
            if (disk_read_block(phys_block, block_buf) != 0)
            {
                return (total_read > 0) ? (ssize_t)total_read : -1;
            }

            memcpy(out + total_read, block_buf + block_offset, to_copy);
        }

        of->offset += (off_t)to_copy;
        total_read += to_copy;
    }

    return (ssize_t)total_read;
}

ssize_t ufs_write(int fd, const void *buf, size_t count)
{
    struct ufs_open_file *of;
    struct ufs_inode inode;
    uint8_t block_buf[UFS_BLOCK_SIZE];
    const uint8_t *in = (const uint8_t *)buf;
    size_t total_written;

    if (disk == NULL)
    {
        errno = EINVAL;
        return -1;
    }

    if (fd < 0 || fd >= UFS_MAX_OPEN_FILES || !open_files[fd].used)
    {
        errno = EBADF;
        return -1;
    }

    if (buf == NULL && count > 0)
    {
        errno = EINVAL;
        return -1;
    }

    of = &open_files[fd];

    /*
     * ملف اتفتح RDONLY فقط لا يجوز نكتب فيه.
     */
    if ((of->flags & UFS_O_RDWR) == UFS_O_RDONLY)
    {
        errno = EBADF;
        return -1;
    }

    if (read_inode((uint32_t)of->inode_number, &inode) != 0)
    {
        return -1;
    }

    /*
     * O_APPEND: كل كتابة لازم تبدأ من نهاية الملف الحالية،
     * حتى لو حصل seek قبلها.
     */
    if (of->flags & UFS_O_APPEND)
    {
        of->offset = (off_t)inode.size;
    }

    total_written = 0;

    while (total_written < count)
    {
        uint64_t logical_block;
        uint32_t phys_block;
        size_t block_offset;
        size_t to_copy;

        logical_block = (uint64_t)of->offset / UFS_BLOCK_SIZE;
        block_offset = (size_t)((uint64_t)of->offset % UFS_BLOCK_SIZE);

        if (file_get_data_block(&inode, logical_block, 1,
                                &phys_block) != 0)
        {
            break;
        }

        to_copy = UFS_BLOCK_SIZE - block_offset;

        if (to_copy > count - total_written)
        {
            to_copy = count - total_written;
        }

        /*
         * لو الكتابة جزئية (مش block كامل)، لازم نقرأ محتوى
         * الـ block الحالي الأول عشان ما نمسحش بايتات موجودة.
         */
        if (block_offset != 0 || to_copy != UFS_BLOCK_SIZE)
        {
            if (disk_read_block(phys_block, block_buf) != 0)
            {
                break;
            }
        }

        memcpy(block_buf + block_offset, in + total_written, to_copy);

        if (disk_write_block(phys_block, block_buf) != 0)
        {
            break;
        }

        of->offset += (off_t)to_copy;
        total_written += to_copy;

        if ((uint64_t)of->offset > inode.size)
        {
            inode.size = (uint64_t)of->offset;
        }
    }

    if (total_written == 0 && count > 0)
    {
        /* لا داعي لكتابة الـ inode لو ما اتغيرش حاجة. */
        return -1;
    }

    inode.modified_at = (int64_t)time(NULL);

    if (write_inode((uint32_t)of->inode_number, &inode) != 0)
    {
        return -1;
    }

    return (ssize_t)total_written;
}

off_t ufs_seek(int fd, off_t offset, int whence)
{
    struct ufs_open_file *of;
    struct ufs_inode inode;
    off_t new_offset;

    if (disk == NULL)
    {
        errno = EINVAL;
        return -1;
    }

    if (fd < 0 || fd >= UFS_MAX_OPEN_FILES || !open_files[fd].used)
    {
        errno = EBADF;
        return -1;
    }

    of = &open_files[fd];

    if (read_inode((uint32_t)of->inode_number, &inode) != 0)
    {
        return -1;
    }

    switch (whence)
    {
    case SEEK_SET:
        new_offset = offset;
        break;

    case SEEK_CUR:
        new_offset = of->offset + offset;
        break;

    case SEEK_END:
        new_offset = (off_t)inode.size + offset;
        break;

    default:
        errno = EINVAL;
        return -1;
    }

    if (new_offset < 0)
    {
        errno = EINVAL;
        return -1;
    }

    of->offset = new_offset;

    return new_offset;
}

int ufs_truncate(const char *path, size_t size)
{
    uint32_t inum;
    struct ufs_inode inode;

    if (disk == NULL)
    {
        errno = EINVAL;
        return -1;
    }

    if (path == NULL)
    {
        errno = EINVAL;
        return -1;
    }

    if (resolve_path(path, &inum) != 0)
    {
        return -1;
    }

    if (read_inode(inum, &inode) != 0)
    {
        return -1;
    }

    if (inode.type != UFS_TYPE_FILE)
    {
        errno = EISDIR;
        return -1;
    }

    if (file_truncate_blocks(&inode, (uint64_t)size) != 0)
    {
        return -1;
    }

    inode.modified_at = (int64_t)time(NULL);

    return write_inode(inum, &inode);
}
int ufs_stat(const char *path, struct ufs_stat *st)
{
    uint32_t inum;
    struct ufs_inode inode;

    if (disk == NULL)
    {
        errno = EINVAL;
        return -1;
    }

    if (path == NULL || st == NULL)
    {
        errno = EINVAL;
        return -1;
    }

    if (resolve_path(path, &inum) != 0)
    {
        return -1;
    }

    if (read_inode(inum, &inode) != 0)
    {
        return -1;
    }

    st->type = (int)inode.type;
    st->size = (size_t)inode.size;

    return 0;
}
