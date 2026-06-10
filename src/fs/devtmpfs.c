/*
 * devtmpfs.c - Minimal device filesystem (stub)
 *
 * Purpose: Provides the /dev/ filesystem populated with kernel device
 *          nodes. Each open file gets a virtual FD (>= 100) and manages
 *          its own internal state.
 *
 * This is a placeholder stub for the mount-point VFS dispatch.
 * Full implementation will be added in Task 4.2.
 */

#include "devtmpfs.h"
#include <stddef.h>

int devtmpfs_open(const char *path, int flags)
{
    (void)path;
    (void)flags;
    return -1; /* Not yet implemented */
}

int devtmpfs_read(int fd, void *buf, uint32_t size)
{
    (void)fd;
    (void)buf;
    (void)size;
    return -1; /* Not yet implemented */
}

int devtmpfs_write(int fd, const void *buf, uint32_t size)
{
    (void)fd;
    (void)buf;
    (void)size;
    return -1; /* Not yet implemented */
}

int devtmpfs_close(int fd)
{
    (void)fd;
    return -1; /* Not yet implemented */
}
