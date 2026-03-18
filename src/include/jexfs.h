/**
 * @file jexfs.h
 * @brief JexFS - Native Filesystem for JexOS.
 *
 * Defines the structure of the JexFS filesystem on disk.
 */

#ifndef JEXFS_H
#define JEXFS_H

#include <stdint.h>

#define JEXFS_MAGIC 0x4A455846 /**< Magic number "JEXF" */
#define BLOCK_SIZE 1024        /**< JexFS block size in bytes. */
#define INODES_PER_BLOCK (BLOCK_SIZE / sizeof(struct jex_inode))
#define DIR_ENTRIES_PER_BLOCK (BLOCK_SIZE / sizeof(struct jex_dir_entry))

/**
 * @struct jex_superblock
 * @brief JexFS Superblock - contains overall filesystem metadata.
 */
struct jex_superblock {
    uint32_t magic;                 /**< Magic number. */
    uint32_t total_blocks;          /**< Total number of blocks. */
    uint32_t total_inodes;          /**< Total number of inodes. */
    uint32_t inode_bitmap_start;    /**< Block index of inode bitmap. */
    uint32_t block_bitmap_start;    /**< Block index of data block bitmap. */
    uint32_t inode_table_start;     /**< Block index of inode table. */
    uint32_t data_start;            /**< Block index where data blocks begin. */
};

/**
 * @struct jex_inode
 * @brief JexFS Inode - represents a file or directory.
 */
struct jex_inode {
    uint16_t mode;      /**< Mode (0: free, 1: file, 2: dir). */
    uint32_t size;      /**< File size in bytes. */
    uint32_t mtime;     /**< Modification time. */
    uint16_t blocks[10]; /**< 10 direct block pointers. */
} __attribute__((packed));

/**
 * @struct jex_dir_entry
 * @brief Directory Entry in JexFS.
 */
struct jex_dir_entry {
    uint16_t inode;     /**< Inode index. */
    char name[14];      /**< Filename (max 14 chars). */
} __attribute__((packed));

extern uint32_t cwd_inode;

/**
 * @brief Initialize JexFS and mount the root directory.
 */
void jexfs_init();

/**
 * @brief Read an inode by its index.
 * @param idx Inode index.
 * @param inode Pointer to the inode structure to fill.
 */
void jexfs_read_inode(uint32_t idx, struct jex_inode* inode);

/**
 * @brief Open a file by name and return its inode index.
 */
int jexfs_open(const char* name);

/**
 * @brief Create a new file.
 */
int jexfs_create(const char* name);

/**
 * @brief Create a new directory.
 */
int jexfs_mkdir(const char* name);

/**
 * @brief Read data from an inode.
 */
int jexfs_read(int inode_idx, void* buffer, uint32_t size, uint32_t offset);

/**
 * @brief Write data to an inode.
 */
int jexfs_write(int inode_idx, const void* buffer, uint32_t size, uint32_t offset);

/**
 * @brief Remove a file or directory.
 */
int jexfs_remove(const char* name);

/**
 * @brief Rename a file.
 */
int jexfs_rename(const char* old_name, const char* new_name);

/**
 * @brief Get the size of a file by inode index.
 */
int jexfs_get_size(int inode_idx);

/**
 * @brief List the contents of a directory.
 */
void jexfs_list_dir(uint32_t inode_idx);

#endif // JEXFS_H
