/**
 * @file fs.c
 * @brief Virtual File System (VFS) implementation.
 *
 * This file provides the generic API for file operations (open, read, write, etc.)
 * by dispatching calls to the underlying filesystem drivers (currently JexFS).
 */

#include "fs.h"
#include "jexfs.h"
#include "devtmpfs.h"
#include "kheap.h"
#include "init.h"
#include <jexos/errno.h>
#include <stddef.h>

extern void terminal_writestring(const char* data);

#define MAX_MOUNTS       4
#define VIRTUAL_FD_START 100

struct mount {
    const char *path;
    int         path_len;
    int       (*open )(const char *path, int flags);
    int       (*read )(int fd, void *buf, uint32_t size);
    int       (*write)(int fd, const void *buf, uint32_t size);
    int       (*close)(int fd);
};

static struct mount mounts[MAX_MOUNTS];
static int mount_count;

/**
 * fs_mount - Register a filesystem at a mount-point path.
 * @path:    Mount-point prefix (e.g. "/sys/").
 * @fstype:  Filesystem driver name.
 * Return: 0 on success, -1 on error.
 *
 * Currently only "devtmpfs" is supported.
 */
int fs_mount(const char *path, const char *fstype)
{
    if (mount_count >= MAX_MOUNTS)
        return -ENOSPC;

    if (strcmp(fstype, "devtmpfs") == 0) {
        mounts[mount_count].path     = path;
        mounts[mount_count].path_len = strlen(path);
        mounts[mount_count].open     = devtmpfs_open;
        mounts[mount_count].read     = devtmpfs_read;
        mounts[mount_count].write    = devtmpfs_write;
        mounts[mount_count].close    = devtmpfs_close;
        mount_count++;
        return 0;
    }

    return -ENODEV;
}

/**
 * fs_find_mount - Find the best-matching mount for a path.
 * @path:  Absolute file path.
 * Return: Pointer to the mount struct, or NULL if no match.
 *
 * Iterates in reverse registration order so that the most-recently
 * mounted filesystem takes priority for a given prefix.
 */
static struct mount *fs_find_mount(const char *path)
{
    for (int i = mount_count - 1; i >= 0; i--) {
        if (strncmp(path, mounts[i].path, mounts[i].path_len) == 0)
            return &mounts[i];
    }
    return NULL;
}

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

device_init(fs_init);

/**
 * @brief Open a file.
 * 
 * @param filename Path to the file.
 * @param flags Access mode (ignored for now).
 * @return File descriptor index on success, -1 on failure.
 */
int fs_open(const char* filename, int flags) {
    /* Dispatch to mount point if path prefix matches */
    struct mount* m = fs_find_mount(filename);
    if (m)
        return m->open(filename, flags);

    /* Fall through to JexFS */
    int fd = -1;
    /* Find a free slot in the file table */
    for (int i = 0; i < MAX_OPEN_FILES; i++) {
        if (!file_table[i].used) {
            fd = i;
            break;
        }
    }
    if (fd < 0) return -ENOMEM;

    /* Ask JexFS to find the file */
    int inode_idx = jexfs_open(filename);
    if (inode_idx < 0) {
        return inode_idx;   /* pass through errno from filesystem */
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
    /* Dispatch to mount if FD is in virtual range */
    if (fd >= VIRTUAL_FD_START) {
        for (int i = 0; i < mount_count; i++) {
            if (mounts[i].read) {
                int ret = mounts[i].read(fd, buffer, size);
                if (ret >= 0) return ret;
            }
        }
        return -ENODEV; /* No mount handled this FD */
    }

    /* Fall through to JexFS */
    if (fd < 0 || fd >= MAX_OPEN_FILES || !file_table[fd].used) return -EBADF;

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
    /* Dispatch to mount if FD is in virtual range */
    if (fd >= VIRTUAL_FD_START) {
        for (int i = 0; i < mount_count; i++) {
            if (mounts[i].write) {
                int ret = mounts[i].write(fd, buffer, size);
                if (ret >= 0) return ret;
            }
        }
        return -ENODEV; /* No mount handled this FD */
    }

    /* Fall through to JexFS */
    if (fd < 0 || fd >= MAX_OPEN_FILES || !file_table[fd].used) return -EBADF;

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
    /* Dispatch to mount if FD is in virtual range */
    if (fd >= VIRTUAL_FD_START) {
        for (int i = 0; i < mount_count; i++) {
            if (mounts[i].close && mounts[i].close(fd) >= 0)
                return;
        }
        return;
    }

    /* Fall through to JexFS */
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
    if (fd < 0 || fd >= MAX_OPEN_FILES || !file_table[fd].used) return -EBADF;
    
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
