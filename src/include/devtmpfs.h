#ifndef DEVTMPS_H
#define DEVTMPS_H

#include <stdint.h>

/*
 * devtmpfs - Minimal device filesystem
 *
 * Purpose: Provides a temporary /dev/ filesystem populated with
 *          kernel-recognized device nodes. Each open file gets a
 *          virtual FD (>= 100) and manages its own internal state.
 *
 * These declarations are stubs for the mount-point VFS dispatch.
 * Full implementation lives in src/fs/devtmpfs.c (Task 4.2).
 */

int devtmpfs_open(const char *path, int flags);
int devtmpfs_read(int fd, void *buf, uint32_t size);
int devtmpfs_write(int fd, const void *buf, uint32_t size);
int devtmpfs_close(int fd);

#endif /* DEVTMPS_H */
