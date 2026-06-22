/**
 * @file jexfs.h
 * @brief JexFS v2 -- Modern on-disk filesystem for JexOS.
 *
 * On-disk layout (1024-byte blocks):
 *   Block 0:     Reserved (boot block)
 *   Block 1:     Superblock
 *   Block 2:     Inode bitmap (1 block = 8192 inode slots)
 *   Blocks 3-10: Block bitmap (8 blocks = 65536 bits)
 *   Blocks 11-53: Inode table (43 blocks, ~1024 inodes x 42 bytes)
 *   Block 54+:   Data blocks (65482 blocks approx 64MB)
 */

#ifndef JEXFS_H
#define JEXFS_H
#include <stdint.h>
#define JEXFS_MAGIC        0x4A455846u
#define BLOCK_SIZE         1024u
#define JEXFS_INODE_SIZE   42u
#define JEXFS_TYPE_MASK    0x0007
#define JEXFS_TYPE_FREE    0
#define JEXFS_TYPE_FILE    1
#define JEXFS_TYPE_DIR     2
#define JEXFS_IRUSR  0x0008
#define JEXFS_IWUSR  0x0010
#define JEXFS_IXUSR  0x0020
#define JEXFS_IRGRP  0x0040
#define JEXFS_IWGRP  0x0080
#define JEXFS_IXGRP  0x0100
#define JEXFS_IROTH  0x0200
#define JEXFS_IWOTH  0x0400
#define JEXFS_IXOTH  0x0800
#define JEXFS_DIRENT_FILE  1
#define JEXFS_DIRENT_DIR   2
#define JEXFS_DIRECT_COUNT        8
#define JEXFS_INDIRECT_ENTRIES   512

struct jex_superblock {
    uint32_t magic;
    uint32_t total_blocks;
    uint32_t total_inodes;
    uint32_t inode_bitmap_start;
    uint32_t block_bitmap_start;
    uint32_t bitmap_blocks;
    uint32_t inode_table_start;
    uint32_t inode_size;
    uint32_t data_start;
} __attribute__((packed));

struct jex_inode {
    uint16_t mode;
    uint16_t uid;
    uint16_t gid;
    uint32_t size;
    uint32_t atime;
    uint32_t mtime;
    uint32_t ctime;
    uint16_t direct_blocks[8];
    uint16_t indirect_block;
    uint16_t double_indirect;
} __attribute__((packed));

struct jex_dir_entry {
    uint16_t inode;
    uint16_t name_len;
    uint8_t  file_type;
    char     name[];
} __attribute__((packed));

extern uint32_t cwd_inode;

void jexfs_init(void);
void jexfs_read_inode(uint32_t idx, struct jex_inode *inode);
void jexfs_write_inode(uint32_t idx, struct jex_inode *inode);
void jexfs_stat(int inode_idx, struct jex_inode *inode);
int  jexfs_open(const char *name);
int  jexfs_read(int inode_idx, void *buf, uint32_t size, uint32_t offset);
int  jexfs_write(int inode_idx, const void *buf, uint32_t size, uint32_t offset);
int  jexfs_create(const char *name);
int  jexfs_mkdir(const char *name);
int  jexfs_remove(const char *name);
int  jexfs_rename(const char *old_name, const char *new_name);
int  jexfs_get_size(int inode_idx);
void jexfs_list_dir(uint32_t inode_idx);
void read_block(uint32_t block, uint8_t *buffer);
void write_block(uint32_t block, const uint8_t *buffer);

#endif /* JEXFS_H */
