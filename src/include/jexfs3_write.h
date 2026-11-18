/**
 * @file jexfs3_write.h
 * @brief JexFS v3 writable path -- shared by the kernel and host tools.
 *
 * Purpose: one implementation of the allocation and ordered-write rules,
 * so the kernel, the crash simulator and any host tooling cannot disagree
 * about what a durable file replacement means.
 *
 * Phase 2b replaces the contents of an existing regular file.  Phase 2c
 * adds file creation and unlinking.  Directories are not created,
 * renamed or removed yet.
 *
 * Thread-safety: the implementation has no static buffers, but the journal
 * helpers it calls keep one.  The kernel serialises calls with its
 * filesystem lock; host tools are single-threaded.
 */

#ifndef JEXFS3_WRITE_H
#define JEXFS3_WRITE_H

#include <stdint.h>
#include <stddef.h>
#include "jexfs3.h"

/**
 * struct jexfs3_write_io - block access and scratch allocation for writes.
 *
 * @read_block: read one 4 KB block
 * @write_block: write one 4 KB block
 * @flush: make prior writes durable; may be NULL
 * @alloc: allocate scratch memory; required
 * @free: release it; required
 * @ctx: caller data passed to every callback
 */
struct jexfs3_write_io {
    int (*read_block)(void *ctx, uint32_t block, void *buf);
    int (*write_block)(void *ctx, uint32_t block, const void *buf);
    int (*flush)(void *ctx);
    void *(*alloc)(size_t size);
    void (*free)(void *ptr);
    void *ctx;
};

/**
 * jexfs3_image_replace_file - replace a regular file's contents.
 *
 * The new data goes to freshly allocated blocks, which are written and
 * flushed before any metadata is logged.  The block bitmap and the inode
 * are then committed as one journal transaction, so a crash leaves either
 * the old file or the complete new one, never a mix.
 *
 * The image must have a journal; writing to one without a journal returns
 * -ENOSYS.  The caller must not let another writer touch the image at the
 * same time.
 *
 * @param sb Validated superblock of the mounted image
 * @param io Block access and scratch allocation
 * @param inode_index Inode of an existing regular file
 * @param data New contents; may be NULL when size is 0
 * @param size New size in bytes
 * @return 0 on success, -EINVAL on a bad inode or image, -EISDIR when the
 *         inode is a directory, -ENOSPC when no block run or journal space
 *         is available, -EFBIG when the size needs more inline extents than
 *         the format has, -ENOSYS without a journal, -EROFS on a read-only
 *         device, -EIO on a device failure
 */
int jexfs3_image_replace_file(const struct jexfs3_superblock *sb,
                        const struct jexfs3_write_io *io,
                        uint32_t inode_index,
                        const void *data, uint64_t size);

/**
 * jexfs3_image_create_file - add an empty regular file to a directory.
 *
 * The inode bitmap, the new inode record, the parent directory block and
 * the parent inode are committed as one journal transaction, so a crash
 * leaves the file either absent or complete.  The parent directory must
 * already have room for the entry; growing a directory is a later phase.
 *
 * @param sb Validated superblock
 * @param io Block access and scratch allocation
 * @param parent_inode Inode of the directory that receives the entry
 * @param name Entry name, 1..JEXFS3_NAME_MAX bytes, no slash
 * @param inode_out Optional newly allocated inode number
 * @return 0 on success, -EEXIST when the name is taken, -ENOENT when the
 *         parent inode is not in use, -ENOTDIR when it is not a directory,
 *         -ENOSPC when no inode or directory slot is free, -ENOSYS without
 *         a journal, -EROFS on a read-only device, -EIO on failure
 */
int jexfs3_image_create_file(const struct jexfs3_superblock *sb,
                             const struct jexfs3_write_io *io,
                             uint32_t parent_inode, const char *name,
                             uint32_t *inode_out);

/**
 * jexfs3_image_unlink_file - remove a regular file from a directory.
 *
 * The directory entry becomes a free entry, the inode bitmap bit is
 * cleared and the file's data and extent blocks return to the block
 * bitmap, all in one journal transaction.  The inode record itself is
 * left on disk, as allocated records are only meaningful with their
 * bitmap bit set.
 *
 * @param sb Validated superblock
 * @param io Block access and scratch allocation
 * @param parent_inode Inode of the directory holding the entry
 * @param name Entry name to remove
 * @return 0 on success, -ENOENT when the name is absent, -ENOTDIR when
 *         the parent is not a directory, -EISDIR for a directory entry,
 *         -ENOSYS without a journal, -EROFS on a read-only device,
 *         -EIO on failure
 */
int jexfs3_image_unlink_file(const struct jexfs3_superblock *sb,
                             const struct jexfs3_write_io *io,
                             uint32_t parent_inode, const char *name);

#endif /* JEXFS3_WRITE_H */
