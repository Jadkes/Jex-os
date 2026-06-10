/*
 * devtmpfs.h - Device and sysfs virtual filesystem
 *
 * Purpose: Provides a virtual filesystem where files are backed by kernel
 *          callbacks rather than on-disk storage.  Used for /sys/ inspection
 *          files and, in future, /dev/ device nodes.
 *
 * Design: Drivers register virtual files via devtmpfs_add_file().  Each file
 *         has a path and a callback table (read, write).  The VFS dispatch
 *         layer (fs.c) routes open/read/write/close calls here when the path
 *         matches a registered mount point.
 *
 * Thread-safety: Not yet — called only during boot and from the shell.
 */

#ifndef DEVTMPS_H
#define DEVTMPS_H

#include <stdint.h>

/** Maximum path length we accept for lookups */
#define DEVTMFS_MAX_PATH 128

/**
 * struct devtmpfs_file - Callback table for one virtual file.
 * @name:  Short filename for display / listing.
 * @read:  Called when user reads the file.  Write content into @buf (max @max_len bytes),
 *         return number of bytes written, or 0 for EOF, or negative on error.
 * @write: Called when user writes to the file.  @buf holds @len bytes.  Return @len on
 *         success, negative on error.  May be NULL for read-only files.
 * @mode:  Unix-style permission bits (e.g. 0444, 0644).
 */
struct devtmpfs_file {
    const char* name;
    int         (*read)(char* buf, int max_len);
    int         (*write)(const char* buf, int len);
    unsigned short mode;
};

/*
 * API for filesystem drivers (called during boot).
 */
int devtmpfs_add_file(const char* path, struct devtmpfs_file* file);
int devtmpfs_init(void);

/*
 * API called by the VFS dispatch layer (fs.c).
 */
int devtmpfs_open(const char* path, int flags);
int devtmpfs_read(int fd, void* buf, uint32_t size);
int devtmpfs_write(int fd, const void* buf, uint32_t size);
int devtmpfs_close(int fd);

#endif /* DEVTMPS_H */
