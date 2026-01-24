#ifndef JEXFS_H
#define JEXFS_H

#include <stdint.h>

#define JEXFS_MAGIC 0x4A455846 // "JEXF"
#define BLOCK_SIZE 1024
#define INODES_PER_BLOCK (BLOCK_SIZE / sizeof(struct jex_inode))
#define DIR_ENTRIES_PER_BLOCK (BLOCK_SIZE / sizeof(struct jex_dir_entry))

struct jex_superblock {
    uint32_t magic;
    uint32_t total_blocks;
    uint32_t total_inodes;
    uint32_t inode_bitmap_start;
    uint32_t block_bitmap_start;
    uint32_t inode_table_start;
    uint32_t data_start;
};

struct jex_inode {
    uint16_t mode;      // 0: free, 1: file, 2: dir
    uint32_t size;
    uint32_t mtime;
    uint16_t blocks[10]; // 10 direct blocks
} __attribute__((packed));

struct jex_dir_entry {
    uint16_t inode;
    char name[14];      
} __attribute__((packed));

extern uint32_t cwd_inode;

void jexfs_init();
void jexfs_read_inode(uint32_t idx, struct jex_inode* inode);
int jexfs_open(const char* name);
int jexfs_create(const char* name);
int jexfs_mkdir(const char* name);
int jexfs_read(int inode_idx, void* buffer, uint32_t size, uint32_t offset);
int jexfs_write(int inode_idx, const void* buffer, uint32_t size, uint32_t offset);
int jexfs_remove(const char* name);
int jexfs_rename(const char* old_name, const char* new_name);
int jexfs_get_size(int inode_idx);
void jexfs_list_dir(uint32_t inode_idx);

#endif