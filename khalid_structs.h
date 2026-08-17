#ifndef UFS_INTERNAL_H
#define UFS_INTERNAL_H

#include "userfs.h"

#include <stdint.h>
#include <sys/types.h>


/* =========================================================
 * Constants
 * ========================================================= */

#define UFS_MAGIC 0x55465331
#define UFS_VERSION 1

#define UFS_BITS_PER_BITMAP_BLOCK (UFS_BLOCK_SIZE * 8)


/* =========================================================
 * On-Disk Structures
 * ========================================================= */

/*
 * Superblock
 *
 * Stored in Block 0.
 * Describes the complete filesystem layout.
 */
struct ufs_superblock
{
    uint32_t magic;
    uint32_t block_size;

    uint64_t total_blocks;
    uint32_t total_inodes;

    /* Inode bitmap */
    uint32_t inode_bitmap_start;
    uint32_t inode_bitmap_blocks;

    /* Block bitmap */
    uint32_t block_bitmap_start;
    uint32_t block_bitmap_blocks;

    /* Inode table */
    uint32_t inode_table_start;
    uint32_t inode_table_blocks;

    /* Data area */
    uint64_t data_start;
    uint64_t data_blocks;

    /* Root directory */
    uint32_t root_inode;

    /* Filesystem version */
    uint32_t version;

    /* Reserved */
    uint8_t reserved[440];
};


/*
 * Inode
 *
 * Every file and directory has one inode.
 */
struct ufs_inode
{
    uint32_t used;
    uint32_t type;

    char name[UFS_MAX_NAME + 1];

    uint64_t size;

    int32_t parent_inode;
    uint32_t permissions;

    int64_t created_at;
    int64_t modified_at;

    uint32_t data_blocks;

    union
    {
        /* ================= DIRECTORY ================= */

        struct
        {
            uint32_t children[44];
            uint32_t child_count;
        } directory;


        /* ================= FILE ================= */

        struct
        {
            uint32_t direct_blocks[10];

            uint32_t indirect_block;

            uint32_t double_indirect_block;

            uint32_t triple_indirect_block;

            uint32_t block_count;
        } file;

    } data;
};


/*
 * Make sure the on-disk structures have
 * exactly the sizes we designed.
 */

_Static_assert(sizeof(struct ufs_inode) == 256,
               "ufs_inode must be exactly 256 bytes");

_Static_assert(sizeof(struct ufs_superblock) == 512,
               "ufs_superblock must be exactly 512 bytes");


/* =========================================================
 * Runtime Structures
 * ========================================================= */

struct ufs_open_file
{
    int used;

    int inode_number;

    int flags;

    off_t offset;
};


#endif