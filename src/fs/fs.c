/**
 * @file fs.c
 * @brief Virtual File System (VFS) implementation.
 *
 * This file provides the generic API for file operations (open, read, write, etc.)
 * by dispatching calls to the underlying filesystem drivers (currently JexFS).
 */

#include "fs.h"
#include "jexfs.h"
#include "kheap.h"
#include <stddef.h>

extern void terminal_writestring(const char* data);

/**
 * @brief System-wide open file table.
 */
file_descriptor_t file_table[MAX_OPEN_FILES];

/**
 * @brief Initialize the VFS and underlying filesystems.
 */
void fs_init() {
    /* Initialize file descriptor table to unused */
    for (int i = 0; i < MAX_OPEN_FILES; i++) {
        file_table[i].used = 0;
    }
    /* Initialize the native JexFS */
    jexfs_init();
}

/**
 * @brief Open a file.
 * 
 * @param filename Path to the file.
 * @param flags Access mode (ignored for now).
 * @return File descriptor index on success, -1 on failure.
 */
int fs_open(const char* filename, int flags) {
    int fd = -1;
    /* Find a free slot in the file table */
    for (int i = 0; i < MAX_OPEN_FILES; i++) {
        if (!file_table[i].used) {
            fd = i;
            break;
        }
    }
    if (fd == -1) return -1;

    /* Ask JexFS to find the file */
    int inode_idx = jexfs_open(filename);
    if (inode_idx == -1) {
        return -1;
    }

    /* Populate the descriptor */
    file_table[fd].used = 1;
    file_table[fd].id = fd;
    file_table[fd].offset = 0;
    file_table[fd].dir_entry_idx = inode_idx; /* We store the inode index here */

    (void)flags;
    return fd;
}

/**
 * @brief Create an empty file.
 */
int fs_create(const char* filename) {
    return jexfs_create(filename);
}

/**
 * @brief Read data from an open file.
 * 
 * @param fd File descriptor index.
 * @param buffer Destination buffer.
 * @param size Number of bytes to read.
 * @return Number of bytes actually read, or -1 on error.
 */
int fs_read(int fd, void* buffer, uint32_t size) {
    if (fd < 0 || fd >= MAX_OPEN_FILES || !file_table[fd].used) return -1;
    
    int bytes = jexfs_read(file_table[fd].dir_entry_idx, buffer, size, file_table[fd].offset);
    if (bytes > 0) {
        file_table[fd].offset += bytes;
    }
    return bytes;
}

/**
 * @brief Write data to an open file.
 * 
 * @param fd File descriptor index.
 * @param buffer Source buffer.
 * @param size Number of bytes to write.
 * @return Number of bytes actually written, or -1 on error.
 */
int fs_write(int fd, const void* buffer, uint32_t size) {
    if (fd < 0 || fd >= MAX_OPEN_FILES || !file_table[fd].used) return -1;
    
    int bytes = jexfs_write(file_table[fd].dir_entry_idx, buffer, size, file_table[fd].offset);
    if (bytes > 0) {
        file_table[fd].offset += bytes;
    }
    return bytes;
}

/**
 * @brief Close an open file descriptor.
 */
void fs_close(int fd) {
    if (fd >= 0 && fd < MAX_OPEN_FILES) {
        file_table[fd].used = 0;
    }
}

/**
 * @brief Reposition the file read/write offset.
 * 
 * @param fd File descriptor index.
 * @param offset New offset.
 * @param whence 0=SEEK_SET, 1=SEEK_CUR, 2=SEEK_END.
 * @return The resulting offset, or -1 on error.
 */
int fs_seek(int fd, int offset, int whence) {
    if (fd < 0 || fd >= MAX_OPEN_FILES || !file_table[fd].used) return -1;
    
    if (whence == 0) { /* SEEK_SET */
        file_table[fd].offset = offset;
    } else if (whence == 1) { /* SEEK_CUR */
        file_table[fd].offset += offset;
    } else if (whence == 2) { /* SEEK_END */
        int size = jexfs_get_size(file_table[fd].dir_entry_idx);
        file_table[fd].offset = size + offset;
    }
    return file_table[fd].offset;
}
