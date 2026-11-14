/**
 * @file jexfs3_journal.h
 * @brief JexFS v3 journal -- replay and log helpers shared by the kernel
 *        and the host tools.
 *
 * Purpose: one implementation of the crash-recovery rules, so the kernel
 * mount, the host verifier and the crash simulator cannot disagree about
 * what a committed transaction means.
 *
 * Thread-safety: the implementation keeps one static block buffer.  It is
 * safe during mount and in single-threaded host tools; a writable kernel
 * path must serialise calls with the filesystem lock.
 */

#ifndef JEXFS3_JOURNAL_H
#define JEXFS3_JOURNAL_H

#include <stdint.h>
#include "jexfs3.h"

/**
 * struct jexfs3_journal_io - block access for the journal.
 *
 * @read_block: read one 4 KB block
 * @write_block: write one 4 KB block; NULL on a read-only device
 * @flush: make prior writes durable; may be NULL
 * @ctx: caller data passed to every callback
 */
struct jexfs3_journal_io {
    int (*read_block)(void *ctx, uint32_t block, void *buf);
    int (*write_block)(void *ctx, uint32_t block, const void *buf);
    int (*flush)(void *ctx);
    void *ctx;
};

/**
 * struct jexfs3_journal_info - what the log contained.
 *
 * @present: image has a journal region
 * @txn_count: data blocks described by the pending transaction, 0 if none
 * @replayed: the transaction was validated and applied
 * @incomplete: a transaction was present but not committed
 */
struct jexfs3_journal_info {
    int present;
    uint32_t txn_count;
    int replayed;
    int incomplete;
};

/**
 * jexfs3_journal_check - validate the journal header.
 *
 * Reads only the header block; it does not inspect or apply a pending
 * transaction.  A clean journal and an empty journal both return 0.
 *
 * @param sb Superblock describing the mounted image
 * @param io Block access
 * @param info Optional output
 * @return 0 when the header is missing (journal_blocks == 0) or valid,
 *         -EINVAL on a corrupt header, -EIO when the device fails
 */
int jexfs3_journal_check(const struct jexfs3_superblock *sb,
                         const struct jexfs3_journal_io *io,
                         struct jexfs3_journal_info *info);

/**
 * jexfs3_journal_replay - apply a committed pending transaction.
 *
 * Validates both the descriptor and the commit block before writing any
 * target.  A transaction without a valid commit is abandoned, never
 * half-applied.  The descriptor is cleared after a successful apply so
 * the next mount has nothing to do.
 *
 * @param sb Superblock describing the mounted image
 * @param io Block access; write_block is required when a transaction
 *           needs applying
 * @param info Optional output describing what was found
 * @return 0 on success (including "nothing to replay"), -EINVAL on a
 *         malformed journal, -EROFS when a write is needed but the device
 *         is read-only, -EIO on a device failure
 */
int jexfs3_journal_replay(const struct jexfs3_superblock *sb,
                          const struct jexfs3_journal_io *io,
                          struct jexfs3_journal_info *info);

/**
 * jexfs3_journal_write - write a transaction into the log.
 *
 * Writes the descriptor, every data block, then the commit block, and
 * flushes.  It does not apply the data to its target blocks; the caller
 * does that after this returns and then calls jexfs3_journal_clear().
 *
 * @param sb Superblock describing the mounted image
 * @param io Block access; write_block and flush are required
 * @param targets Target block of each data block
 * @param data One 4 KB image per target
 * @param count Number of blocks, 1..JEXFS3_JOURNAL_MAX_TXN
 * @return 0 on success, -EINVAL on bad arguments or a journal too small,
 *         -EROFS on a read-only device, -EIO on a device failure
 */
int jexfs3_journal_write(const struct jexfs3_superblock *sb,
                         const struct jexfs3_journal_io *io,
                         const uint32_t *targets,
                         const uint8_t *const *data,
                         uint32_t count);

/**
 * jexfs3_journal_clear - mark the log empty.
 *
 * Clears the descriptor magic of the pending transaction.  Must be called
 * after the transaction's blocks have been written to their real homes.
 *
 * @param sb Superblock describing the mounted image
 * @param io Block access; write_block and flush are required
 * @return 0 on success, -EROFS on a read-only device, -EIO on failure
 */
int jexfs3_journal_clear(const struct jexfs3_superblock *sb,
                         const struct jexfs3_journal_io *io);

#endif /* JEXFS3_JOURNAL_H */
