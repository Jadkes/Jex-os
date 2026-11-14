/**
 * @file jexfs3_journal.c
 * @brief JexFS v3 journal replay and log writer.
 *
 * Shared by the kernel mount, jexfsck3 and the crash simulator.  The
 * format is documented in jexfs3.h: one descriptor, N data blocks and a
 * commit block, with a crc32c that covers the whole transaction.
 *
 * A transaction is applied only when the descriptor and the commit block
 * both validate.  The commit block is written last, after the data
 * blocks, so a crash that loses the commit leaves the previous on-disk
 * state untouched.
 *
 * Thread-safety: one static 4 KB buffer is reused for every operation.
 * Mount and host tools are single-threaded; a writable kernel path must
 * hold the filesystem lock around these calls.
 */

#include "jexfs3_journal.h"
#include <string.h>
#include <jexos/errno.h>

static uint8_t journal_block[JEXFS3_BLOCK_SIZE] __attribute__((aligned(16)));

static int read_transaction(const struct jexfs3_superblock *sb,
                            const struct jexfs3_journal_io *io,
                            struct jexfs3_journal_desc *desc,
                            uint32_t *data_start);

static uint32_t journal_blocks_needed(uint32_t count)
{
    return count + 3u;   /* header + descriptor + data + commit */
}

static int journal_read(const struct jexfs3_journal_io *io, uint32_t block,
                        void *buf)
{
    if (!io || !io->read_block)
        return -EINVAL;
    if (io->read_block(io->ctx, block, buf) != 0)
        return -EIO;
    return 0;
}

static int journal_write(const struct jexfs3_journal_io *io, uint32_t block,
                         const void *buf)
{
    if (!io || !io->write_block)
        return -EROFS;
    if (io->write_block(io->ctx, block, buf) != 0)
        return -EIO;
    return 0;
}

static int journal_flush(const struct jexfs3_journal_io *io)
{
    if (io && io->flush && io->flush(io->ctx) != 0)
        return -EIO;
    return 0;
}

/*
 * header_check - validate the immutable journal header.
 *
 * journal_blocks == 0 means the image has no journal; nothing to check.
 */
static int header_check(const struct jexfs3_superblock *sb,
                        const struct jexfs3_journal_io *io,
                        struct jexfs3_journal_header *out)
{
    struct jexfs3_journal_header header;
    int rc;

    if (!sb || !io)
        return -EINVAL;
    if (sb->journal_blocks == 0) {
        if (out)
            memset(out, 0, sizeof(*out));
        return 0;
    }
    if (sb->journal_blocks < JEXFS3_JOURNAL_MIN_BLOCKS ||
        (uint64_t)sb->journal_start + sb->journal_blocks >
            (uint64_t)sb->total_blocks)
        return -EINVAL;

    rc = journal_read(io, sb->journal_start, journal_block);
    if (rc != 0)
        return rc;
    memcpy(&header, journal_block, sizeof(header));

    if (header.magic != JEXFS3_JOURNAL_MAGIC ||
        header.version != JEXFS3_JOURNAL_VERSION ||
        header.block_size != JEXFS3_BLOCK_SIZE ||
        header.journal_blocks != sb->journal_blocks)
        return -EINVAL;
    if (jexfs3_crc_zeroed(journal_block, JEXFS3_BLOCK_SIZE,
                          JEXFS3_JOURNAL_HEADER_CHECKSUM_OFF) != header.checksum)
        return -EINVAL;

    if (out)
        *out = header;
    return 0;
}

/*
 * targets_ok - reject a transaction that could corrupt the journal or
 * point outside the image.  Duplicate targets would make the apply order
 * ambiguous, so they are errors too.
 */
static int targets_ok(const struct jexfs3_superblock *sb, uint32_t count,
                      const uint32_t *targets)
{
    uint32_t i;

    if (count == 0 || count > JEXFS3_JOURNAL_MAX_TXN ||
        count > sb->journal_blocks - 3u)
        return -EINVAL;
    for (i = 0; i < count; i++) {
        uint32_t j;

        if (targets[i] >= sb->total_blocks)
            return -EINVAL;
        if (targets[i] >= sb->journal_start &&
            targets[i] < sb->journal_start + sb->journal_blocks)
            return -EINVAL;
        for (j = 0; j < i; j++)
            if (targets[j] == targets[i])
                return -EINVAL;
    }
    return 0;
}

int jexfs3_journal_check(const struct jexfs3_superblock *sb,
                         const struct jexfs3_journal_io *io,
                         struct jexfs3_journal_info *info)
{
    struct jexfs3_journal_header header;
    struct jexfs3_journal_desc desc;
    uint32_t data_start;
    int rc;

    if (info)
        memset(info, 0, sizeof(*info));
    rc = header_check(sb, io, &header);
    if (rc != 0)
        return rc;
    if (sb->journal_blocks == 0)
        return 0;
    if (info)
        info->present = 1;

    rc = read_transaction(sb, io, &desc, &data_start);
    if (rc < 0)
        return rc;
    if (rc > 0 && info)
        info->txn_count = desc.count;
    if (rc == 0 && info)
        info->incomplete = desc.magic == JEXFS3_JOURNAL_DESC_MAGIC;
    return 0;
}

/*
 * read_transaction - validate the pending transaction without touching
 * any target block.  Returns 1 when committed, 0 when there is none or it
 * is incomplete, and a negative errno on a malformed journal.
 */
static int read_transaction(const struct jexfs3_superblock *sb,
                            const struct jexfs3_journal_io *io,
                            struct jexfs3_journal_desc *desc,
                            uint32_t *data_start)
{
    struct jexfs3_journal_commit commit;
    uint32_t targets[JEXFS3_JOURNAL_MAX_TXN];
    uint32_t commit_block;
    uint32_t crc;
    uint32_t i;
    int rc;

    *data_start = sb->journal_start + 2u;

    rc = journal_read(io, sb->journal_start + 1u, journal_block);
    if (rc != 0)
        return rc;
    memcpy(desc, journal_block, sizeof(*desc));
    if (desc->magic != JEXFS3_JOURNAL_DESC_MAGIC)
        return 0;

    /* A torn descriptor is an abandoned transaction, not a corrupt
     * filesystem: the commit block is the authority on whether any of it
     * should be applied.  A valid commit also proves the descriptor
     * bytes, because its crc covers them.  The struct is packed, so the
     * target list is copied out before validation. */
    memcpy(targets, desc->targets, sizeof(targets));
    if (targets_ok(sb, desc->count, targets) != 0)
        return 0;

    /* Hash the descriptor and every data block, as the commit did. */
    crc = jexfs3_crc32c(0, journal_block, JEXFS3_BLOCK_SIZE);
    for (i = 0; i < desc->count; i++) {
        rc = journal_read(io, *data_start + i, journal_block);
        if (rc != 0)
            return rc;
        crc = jexfs3_crc32c(crc, journal_block, JEXFS3_BLOCK_SIZE);
    }

    commit_block = *data_start + desc->count;
    rc = journal_read(io, commit_block, journal_block);
    if (rc != 0)
        return rc;
    memcpy(&commit, journal_block, sizeof(commit));
    if (commit.magic != JEXFS3_JOURNAL_COMMIT_MAGIC)
        return 0;
    if (commit.count != desc->count || commit.checksum != crc)
        return 0;
    return 1;
}

int jexfs3_journal_replay(const struct jexfs3_superblock *sb,
                          const struct jexfs3_journal_io *io,
                          struct jexfs3_journal_info *info)
{
    struct jexfs3_journal_desc desc;
    uint32_t data_start;
    uint32_t i;
    int rc;

    if (info)
        memset(info, 0, sizeof(*info));
    rc = header_check(sb, io, NULL);
    if (rc != 0)
        return rc;
    if (info)
        info->present = sb->journal_blocks != 0;
    if (sb->journal_blocks == 0)
        return 0;

    rc = read_transaction(sb, io, &desc, &data_start);
    if (rc < 0)
        return rc;
    if (rc == 0) {
        /* No committed transaction: nothing to apply.  An abandoned
         * descriptor is left alone; the next writer overwrites it. */
        if (info)
            info->incomplete = desc.magic == JEXFS3_JOURNAL_DESC_MAGIC;
        return 0;
    }
    if (info)
        info->txn_count = desc.count;
    if (!io->write_block)
        return -EROFS;

    /* Apply only after the whole transaction validated. */
    for (i = 0; i < desc.count; i++) {
        rc = journal_read(io, data_start + i, journal_block);
        if (rc != 0)
            return rc;
        rc = journal_write(io, desc.targets[i], journal_block);
        if (rc != 0)
            return rc;
    }
    rc = journal_flush(io);
    if (rc != 0)
        return rc;

    /* The descriptor is the "transaction pending" flag.  A crash before
     * this clear only makes the next mount apply the same blocks again. */
    rc = jexfs3_journal_clear(sb, io);
    if (rc == 0 && info)
        info->replayed = 1;
    return rc;
}

int jexfs3_journal_write(const struct jexfs3_superblock *sb,
                         const struct jexfs3_journal_io *io,
                         const uint32_t *targets,
                         const uint8_t *const *data,
                         uint32_t count)
{
    struct jexfs3_journal_desc desc;
    struct jexfs3_journal_commit commit;
    uint32_t data_start;
    uint32_t commit_block;
    uint32_t crc;
    uint32_t i;
    int rc;

    if (!sb || !io || !targets || !data)
        return -EINVAL;
    if (!io->write_block)
        return -EROFS;
    if (sb->journal_blocks == 0 ||
        sb->journal_blocks < journal_blocks_needed(count))
        return -EINVAL;
    if (targets_ok(sb, count, targets) != 0)
        return -EINVAL;
    for (i = 0; i < count; i++)
        if (!data[i])
            return -EINVAL;

    data_start = sb->journal_start + 2u;
    commit_block = data_start + count;

    memset(&desc, 0, sizeof(desc));
    desc.magic = JEXFS3_JOURNAL_DESC_MAGIC;
    desc.count = count;
    memcpy(desc.targets, targets, count * sizeof(uint32_t));

    memset(journal_block, 0, sizeof(journal_block));
    memcpy(journal_block, &desc, sizeof(desc));
    rc = journal_write(io, sb->journal_start + 1u, journal_block);
    if (rc != 0)
        return rc;

    crc = jexfs3_crc32c(0, journal_block, JEXFS3_BLOCK_SIZE);
    for (i = 0; i < count; i++) {
        rc = journal_write(io, data_start + i, data[i]);
        if (rc != 0)
            return rc;
        crc = jexfs3_crc32c(crc, data[i], JEXFS3_BLOCK_SIZE);
    }

    memset(&commit, 0, sizeof(commit));
    commit.magic = JEXFS3_JOURNAL_COMMIT_MAGIC;
    commit.count = count;
    commit.checksum = crc;
    memset(journal_block, 0, sizeof(journal_block));
    memcpy(journal_block, &commit, sizeof(commit));
    rc = journal_write(io, commit_block, journal_block);
    if (rc != 0)
        return rc;
    return journal_flush(io);
}

int jexfs3_journal_clear(const struct jexfs3_superblock *sb,
                         const struct jexfs3_journal_io *io)
{
    struct jexfs3_journal_desc desc;
    int rc;

    if (!sb || !io || !io->write_block)
        return -EROFS;
    if (sb->journal_blocks < JEXFS3_JOURNAL_MIN_BLOCKS)
        return 0;

    rc = journal_read(io, sb->journal_start + 1u, journal_block);
    if (rc != 0)
        return rc;
    memcpy(&desc, journal_block, sizeof(desc));
    if (desc.magic != JEXFS3_JOURNAL_DESC_MAGIC)
        return 0;

    desc.magic = 0;
    memset(journal_block, 0, sizeof(journal_block));
    memcpy(journal_block, &desc, sizeof(desc));
    rc = journal_write(io, sb->journal_start + 1u, journal_block);
    if (rc != 0)
        return rc;
    return journal_flush(io);
}
