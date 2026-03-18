/**
 * @file fs.h
 * @brief Virtual File System (VFS) interface.
 *
 * Provides a generic interface for file operations across different filesystems.
 */

#ifndef FS_H
#define FS_H

#include <stdint.h>
#include "fat12.h"
#include "config.h"

/**
 * @struct file_descriptor_t
 * @brief Represents an open file in the system.
 */
typedef struct {
    int id;                     /**< Unique file descriptor ID. */
    int used;                   /**< Flag indicating if this descriptor is in use. */
    uint32_t offset;            /**< Current read/write offset (cursor). */
    uint32_t file_size;         /**< Total size of the file in bytes. */
    uint32_t data_start_sector; /**< Starting sector on disk (for simple FS). */
    fat12_entry_t dir_entry;    /**< FAT12 directory entry cache. */
    uint32_t dir_entry_idx;     /**< Inode or directory entry index. */
} file_descriptor_t;

/**
 * @brief Initialize the Virtual File System and register sub-filesystems.
 */
void fs_init();

/**
 * @brief Create a new file.
 * @param filename Name of the file to create.
 * @return 0 on success, -1 on failure.
 */
int fs_create(const char* filename);

/**
 * @brief Open an existing file.
 * @param filename Name of the file to open.
 * @param flags Access flags (O_RDONLY, O_WRONLY, etc.).
 * @return File descriptor ID, or -1 on failure.
 */
int fs_open(const char* filename, int flags);

/**
 * @brief Read data from an open file.
 * @param fd File descriptor ID.
 * @param buffer Pointer to the destination buffer.
 * @param size Number of bytes to read.
 * @return Number of bytes read, or -1 on failure.
 */
int fs_read(int fd, void* buffer, uint32_t size);

/**
 * @brief Write data to an open file.
 * @param fd File descriptor ID.
 * @param buffer Pointer to the source data.
 * @param size Number of bytes to write.
 * @return Number of bytes written, or -1 on failure.
 */
int fs_write(int fd, const void* buffer, uint32_t size);

/**
 * @brief Close an open file descriptor.
 * @param fd File descriptor ID.
 */
void fs_close(int fd);

/**
 * @brief Change the current read/write offset in a file.
 * @param fd File descriptor ID.
 * @param offset New offset relative to 'whence'.
 * @param whence Base position (SEEK_SET, SEEK_CUR, SEEK_END).
 * @return The new offset, or -1 on failure.
 */
int fs_seek(int fd, int offset, int whence);

#endif // FS_H
