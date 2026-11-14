/**
 * @file jexfs3_fs.h
 * @brief JexFS v3 kernel-side read API.
 *
 * The on-disk format lives in jexfs3.h; this header is the driver
 * interface.  Phase 1 is read-only: a device supplies a block read
 * callback and the driver validates the superblock, inodes, extents and
 * data checksums before serving lookups, directory iteration and file
 * reads.
 */

#ifndef JEXFS3_FS_H
#define JEXFS3_FS_H

#include <stdint.h>
#include "jexfs3.h"

/**
 * struct jexfs3_device - backing store for a v3 mount.
 *
 * @read_block: read one 4 KB block; return 0 on success, negative on error
 * @ctx: caller data passed through to read_block
 * @media_bytes: device size in bytes, or 0 when unknown
 */
struct jexfs3_device {
    int (*read_block)(void *ctx, uint32_t block, void *buf);
    void *ctx;
    uint64_t media_bytes;
};

/**
 * jexfs3_mount - validate a v3 image and mount it read-only.
 *
 * Reads the primary and backup superblocks, checks their checksums and
 * layout, and validates the root inode.  On success the driver serves
 * reads until jexfs3_unmount().
 *
 * @param dev Device description; must stay valid while mounted.
 * @return 0 on success, -EBUSY if already mounted, -EINVAL on a bad
 *         image, -EIO when the device fails.
 */
int jexfs3_mount(const struct jexfs3_device *dev);

/** jexfs3_unmount - drop the current mount (no-op when not mounted). */
void jexfs3_unmount(void);

/** jexfs3_mounted - return 1 when an image is mounted, else 0. */
int jexfs3_mounted(void);

/**
 * jexfs3_superblock - copy the mounted superblock out.
 * @param out Destination (256 bytes)
 * @return 0 on success, -ENODEV when not mounted
 */
int jexfs3_superblock(struct jexfs3_superblock *out);

/**
 * jexfs3_lookup - resolve an absolute path to an inode number.
 *
 * @param path Absolute path using '/'
 * @param inode_out Receives the inode number
 * @return 0 on success, -ENOENT, -ENOTDIR, -ENAMETOOLONG, -ENODEV
 */
int jexfs3_lookup(const char *path, uint32_t *inode_out);

/**
 * jexfs3_read_file - read bytes from a regular file.
 *
 * @param inode Inode returned by jexfs3_lookup()
 * @param buf Destination
 * @param size Bytes requested
 * @param offset Byte offset into the file
 * @return Bytes read (may be short at EOF), or a negative errno
 */
int jexfs3_read_file(uint32_t inode, void *buf, uint32_t size,
                     uint64_t offset);

/**
 * jexfs3_readdir - iterate one directory entry at a time.
 *
 * @param inode Directory inode
 * @param offset Cookie: start at 0 and pass back unchanged each call
 * @param name Destination for the NUL-terminated name
 * @param name_cap Capacity of name (must be at least 2)
 * @param child_out Receives the entry's inode number
 * @param type_out Receives JEXFS3_DIRENT_FILE or JEXFS3_DIRENT_DIR
 * @return 1 on an entry, 0 at end of directory, negative errno on error
 */
int jexfs3_readdir(uint32_t inode, uint64_t *offset, char *name,
                   uint32_t name_cap, uint32_t *child_out, uint8_t *type_out);

/**
 * jexfs3_verify - full read-only consistency check.
 *
 * Walks every allocated inode, validates its checksum and extent list,
 * recomputes its data crc, checks directory entry chains and child
 * types, and confirms that every allocated inode is reachable from the
 * root.  Intended for tests and mkfs validation, not the hot path.
 *
 * @return 0 when clean, negative errno on the first inconsistency.
 */
int jexfs3_verify(void);

#endif /* JEXFS3_FS_H */
