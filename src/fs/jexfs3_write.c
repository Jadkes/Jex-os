/**
 * @file jexfs3_write.c
 * @brief JexFS v3 writable path -- allocate, order data, journal metadata.
 *
 * Purpose: one implementation of the rules a durable file replacement must
 * follow.  The kernel, the crash simulator and host tooling all call this
 * file, so a change to allocation or write ordering cannot make them drift.
 *
 * Design:
 *   - New data always goes to freshly allocated blocks.  Blocks still
 *     referenced by the old inode are never overwritten, because a crash
 *     before the journal commit must leave the old file readable.
 *   - Data blocks are written and flushed before the metadata transaction
 *     is logged (the ordered-data rule).
 *   - The block bitmap and the inode are committed together, so the
 *     allocation and the extent swap become visible atomically.
 *   - The replacement is contiguous and uses inline extents only, split
 *     into 65535-block chunks.  A file large enough to need more than four
 *     inline extents is rejected rather than writing an extent leaf.
 *
 * Thread-safety: no static buffers here, but jexfs3_journal.c keeps one.
 * The kernel serialises calls with its filesystem lock; host tools are
 * single-threaded.
 */

#include "jexfs3_write.h"
#include "jexfs3_journal.h"
#include <string.h>
#include <jexos/errno.h>

#define JEXFS3_BLOCK_SHIFT 12u
#define JEXFS3_BLOCK_MASK  (JEXFS3_BLOCK_SIZE - 1u)
#define BITMAP_BITS        (JEXFS3_BLOCK_SIZE * 8u)
#define MAX_EXTENT_BLOCKS  65535u

struct bitmap_update {
    uint32_t block;
    uint8_t *image;
};

struct write_state {
    const struct jexfs3_superblock *sb;
    const struct jexfs3_write_io *io;
    struct bitmap_update bitmap[JEXFS3_JOURNAL_MAX_TXN];
    uint32_t bitmap_count;
    uint8_t *scratch;       /* bitmap scans and data staging */
    uint8_t *leaf;          /* old extent leaf, if the inode had one */
    uint8_t *inode_image;   /* inode-table block being edited */
    uint32_t inode_block;
    uint32_t inode_offset;
};

static uint64_t blocks_for_size(uint64_t size)
{
    return (size + JEXFS3_BLOCK_SIZE - 1u) >> JEXFS3_BLOCK_SHIFT;
}

static int read_block(struct write_state *st, uint32_t block, void *buf)
{
    if (!st->io->read_block)
        return -EINVAL;
    if (st->io->read_block(st->io->ctx, block, buf) != 0)
        return -EIO;
    return 0;
}

static int write_block(struct write_state *st, uint32_t block,
                       const void *buf)
{
    if (!st->io->write_block)
        return -EROFS;
    if (st->io->write_block(st->io->ctx, block, buf) != 0)
        return -EIO;
    return 0;
}

static int flush_device(struct write_state *st)
{
    if (st->io->flush && st->io->flush(st->io->ctx) != 0)
        return -EIO;
    return 0;
}

static uint32_t bitmap_block_of(const struct jexfs3_superblock *sb,
                                uint32_t block)
{
    return sb->block_bitmap_start + block / BITMAP_BITS;
}

static int bitmap_image_get(struct write_state *st, uint32_t block,
                            uint8_t **out)
{
    uint8_t *image;
    uint32_t i;
    int rc;

    for (i = 0; i < st->bitmap_count; i++) {
        if (st->bitmap[i].block == block) {
            *out = st->bitmap[i].image;
            return 0;
        }
    }
    /* One journal slot is reserved for the inode-table block. */
    if (st->bitmap_count >= JEXFS3_JOURNAL_MAX_TXN - 1u)
        return -ENOSPC;

    image = (uint8_t *)st->io->alloc(JEXFS3_BLOCK_SIZE);
    if (!image)
        return -ENOMEM;
    rc = read_block(st, block, image);
    if (rc != 0) {
        st->io->free(image);
        return rc;
    }
    st->bitmap[st->bitmap_count].block = block;
    st->bitmap[st->bitmap_count].image = image;
    st->bitmap_count++;
    *out = image;
    return 0;
}

static int bitmap_read_bit(struct write_state *st, uint32_t block, int *used)
{
    uint32_t bit = block % BITMAP_BITS;
    int rc;

    rc = read_block(st, bitmap_block_of(st->sb, block), st->scratch);
    if (rc != 0)
        return rc;
    *used = (st->scratch[bit / 8u] >> (bit % 8u)) & 1u;
    return 0;
}

/*
 * bitmap_stage_change - change one bit in a staging copy of the bitmap.
 *
 * The caller states what the on-disk bit must already be by asking for the
 * opposite value.  A bit that is already in the requested state means the
 * bitmap disagrees with the inode or the allocator, so the write stops
 * before anything is logged.
 */
static int bitmap_stage_change(struct write_state *st, uint32_t block,
                               int new_used)
{
    uint8_t *image;
    uint32_t bit = block % BITMAP_BITS;
    uint32_t byte = bit / 8u;
    uint32_t mask = 1u << (bit % 8u);
    int current;
    int rc;

    rc = bitmap_image_get(st, bitmap_block_of(st->sb, block), &image);
    if (rc != 0)
        return rc;
    current = (image[byte] & mask) ? 1 : 0;
    if (current == new_used)
        return -EINVAL;
    if (new_used)
        image[byte] |= (uint8_t)mask;
    else
        image[byte] &= (uint8_t)~mask;
    return 0;
}

static int extent_range_ok(const struct jexfs3_superblock *sb,
                           const struct jexfs3_extent *ext)
{
    uint64_t end;

    if (ext->length == 0 || ext->flags != 0)
        return -EINVAL;
    if (ext->physical < sb->first_data_block)
        return -EINVAL;
    end = (uint64_t)ext->physical + ext->length;
    if (end > sb->backup_block)
        return -EINVAL;
    return 0;
}

/*
 * load_inode_extents - validate the old inode's extent list.
 *
 * The leaf extent block, when present, is left in st->leaf so the free
 * pass can walk it without a second read.  The caller must not touch
 * st->leaf until the old blocks have been released.
 */
static int load_inode_extents(struct write_state *st,
                              const struct jexfs3_inode *inode)
{
    const struct jexfs3_superblock *sb = st->sb;
    uint64_t expected64 = blocks_for_size(inode->size);
    uint32_t expected;
    uint32_t logical = 0;
    uint32_t total = 0;
    uint32_t i;
    int rc;

    if (inode->size > (uint64_t)sb->total_blocks * JEXFS3_BLOCK_SIZE)
        return -EINVAL;
    if (expected64 > sb->total_blocks)
        return -EINVAL;
    expected = (uint32_t)expected64;
    if (inode->extent_depth > 1)
        return -EINVAL;

    if (inode->extent_depth == 1) {
        const struct jexfs3_extent_block *eb;
        uint32_t crc;

        if (inode->extent_root < sb->first_data_block ||
            inode->extent_root >= sb->backup_block)
            return -EINVAL;
        rc = read_block(st, inode->extent_root, st->leaf);
        if (rc != 0)
            return rc;
        eb = (const struct jexfs3_extent_block *)st->leaf;
        if (eb->magic != JEXFS3_EXTENT_MAGIC || eb->depth != 1 ||
            eb->count == 0 || eb->count > JEXFS3_EXTENT_ENTRIES)
            return -EINVAL;
        crc = jexfs3_crc_zeroed(st->leaf, JEXFS3_BLOCK_SIZE,
                                JEXFS3_EXTENT_CHECKSUM_OFF);
        if (crc != eb->checksum)
            return -EINVAL;

        for (i = 0; i < eb->count; i++) {
            const struct jexfs3_extent *ext = &eb->entries[i];

            if (extent_range_ok(sb, ext) != 0 || ext->logical != logical)
                return -EINVAL;
            logical += ext->length;
            total += ext->length;
        }
    } else {
        if (inode->extent_count > JEXFS3_INLINE_EXTENTS)
            return -EINVAL;
        for (i = 0; i < inode->extent_count; i++) {
            const struct jexfs3_extent *ext = &inode->inline_extents[i];

            if (extent_range_ok(sb, ext) != 0 || ext->logical != logical)
                return -EINVAL;
            logical += ext->length;
            total += ext->length;
        }
    }

    if (total != expected)
        return -EINVAL;
    if (inode->blocks != (uint64_t)total + (inode->extent_depth ? 1u : 0u))
        return -EINVAL;
    return 0;
}

/*
 * check_old_blocks_used - every block the old inode references must be
 * marked used.  Doing this before allocation keeps a corrupt bitmap from
 * letting a freshly allocated block be freed again by the old extent list.
 */
static int check_old_blocks_used(struct write_state *st,
                                 const struct jexfs3_inode *inode)
{
    uint32_t i;

    if (inode->extent_depth == 1) {
        const struct jexfs3_extent_block *eb =
            (const struct jexfs3_extent_block *)st->leaf;

        for (i = 0; i < eb->count; i++) {
            const struct jexfs3_extent *ext = &eb->entries[i];
            uint32_t b;

            for (b = 0; b < ext->length; b++) {
                int used;

                if (bitmap_read_bit(st, ext->physical + b, &used) != 0 ||
                    !used)
                    return -EINVAL;
            }
        }
        {
            int used;

            if (bitmap_read_bit(st, inode->extent_root, &used) != 0 ||
                !used)
                return -EINVAL;
        }
        return 0;
    }

    for (i = 0; i < inode->extent_count; i++) {
        const struct jexfs3_extent *ext = &inode->inline_extents[i];
        uint32_t b;

        for (b = 0; b < ext->length; b++) {
            int used;

            if (bitmap_read_bit(st, ext->physical + b, &used) != 0 || !used)
                return -EINVAL;
        }
    }
    return 0;
}

static int alloc_run(struct write_state *st, uint32_t count, uint32_t *start)
{
    const struct jexfs3_superblock *sb = st->sb;
    uint32_t map_block = UINT32_MAX;
    uint32_t run = 0;
    uint32_t run_start = 0;
    uint32_t b;
    int rc;

    if (count == 0) {
        *start = 0;
        return 0;
    }

    for (b = sb->first_data_block; b < sb->backup_block; b++) {
        uint32_t mb = bitmap_block_of(sb, b);
        uint32_t bit;
        int used;

        if (mb != map_block) {
            rc = read_block(st, mb, st->scratch);
            if (rc != 0)
                return rc;
            map_block = mb;
        }
        bit = b % BITMAP_BITS;
        used = (st->scratch[bit / 8u] >> (bit % 8u)) & 1u;
        if (!used) {
            if (run == 0)
                run_start = b;
            run++;
            if (run == count) {
                *start = run_start;
                return 0;
            }
        } else {
            run = 0;
        }
    }
    return -ENOSPC;
}

static int release_old_blocks(struct write_state *st,
                              const struct jexfs3_inode *inode)
{
    uint32_t i;
    int rc;

    if (inode->extent_depth == 1) {
        const struct jexfs3_extent_block *eb =
            (const struct jexfs3_extent_block *)st->leaf;

        for (i = 0; i < eb->count; i++) {
            const struct jexfs3_extent *ext = &eb->entries[i];
            uint32_t b;

            for (b = 0; b < ext->length; b++) {
                rc = bitmap_stage_change(st, ext->physical + b, 0);
                if (rc != 0)
                    return rc;
            }
        }
        return bitmap_stage_change(st, inode->extent_root, 0);
    }

    for (i = 0; i < inode->extent_count; i++) {
        const struct jexfs3_extent *ext = &inode->inline_extents[i];
        uint32_t b;

        for (b = 0; b < ext->length; b++) {
            rc = bitmap_stage_change(st, ext->physical + b, 0);
            if (rc != 0)
                return rc;
        }
    }
    return 0;
}

static int claim_new_blocks(struct write_state *st, uint32_t start,
                            uint32_t count)
{
    uint32_t i;

    for (i = 0; i < count; i++) {
        int rc = bitmap_stage_change(st, start + i, 1);

        if (rc != 0)
            return rc;
    }
    return 0;
}

static uint32_t data_crc(const uint8_t *data, uint64_t size)
{
    uint32_t crc = 0;
    uint64_t pos = 0;

    while (pos < size) {
        uint32_t chunk = JEXFS3_BLOCK_SIZE;

        if (size - pos < chunk)
            chunk = (uint32_t)(size - pos);
        crc = jexfs3_crc32c(crc, data + pos, chunk);
        pos += chunk;
    }
    return crc;
}

static void build_inline_extents(struct jexfs3_inode *inode, uint32_t start,
                                 uint32_t count)
{
    uint32_t left = count;
    uint32_t physical = start;
    uint32_t logical = 0;
    uint32_t extents = 0;

    memset(inode->inline_extents, 0, sizeof(inode->inline_extents));
    while (left > 0) {
        uint16_t length = left > MAX_EXTENT_BLOCKS
                          ? (uint16_t)MAX_EXTENT_BLOCKS : (uint16_t)left;

        inode->inline_extents[extents].logical = logical;
        inode->inline_extents[extents].physical = physical;
        inode->inline_extents[extents].length = length;
        logical += length;
        physical += length;
        left -= length;
        extents++;
    }
    inode->extent_count = (uint16_t)extents;
}

/*
 * build_inode_image - rewrite the inode record inside its table block.
 *
 * The old record is copied forward so mode, links and ownership survive.
 * The checksum is recomputed over the modified block image, and the record
 * keeps its original position.
 */
static int build_inode_image(struct write_state *st,
                             const struct jexfs3_inode *old,
                             uint64_t size, uint32_t start, uint32_t count,
                             uint32_t crc)
{
    struct jexfs3_inode next;
    uint8_t *record = st->inode_image + st->inode_offset;
    uint32_t valid_crc;

    memcpy(&next, record, JEXFS3_INODE_SIZE);
    valid_crc = jexfs3_crc_zeroed(record, JEXFS3_INODE_SIZE,
                                  JEXFS3_INODE_CHECKSUM_OFF);
    if (valid_crc != next.checksum)
        return -EINVAL;

    next.size = size;
    next.blocks = count;
    next.data_crc = crc;
    next.extent_depth = 0;
    next.extent_root = 0;
    next.generation = old->generation + 1u;
    build_inline_extents(&next, start, count);
    next.checksum = jexfs3_crc_zeroed(&next, JEXFS3_INODE_SIZE,
                                      JEXFS3_INODE_CHECKSUM_OFF);
    memcpy(record, &next, JEXFS3_INODE_SIZE);
    return 0;
}

static int write_data(struct write_state *st, const uint8_t *data,
                      uint64_t size, uint32_t start, uint32_t count)
{
    uint32_t i;
    int rc;

    for (i = 0; i < count; i++) {
        uint64_t offset = (uint64_t)i * JEXFS3_BLOCK_SIZE;
        uint64_t left = size - offset;
        uint32_t chunk = left > JEXFS3_BLOCK_SIZE
                         ? JEXFS3_BLOCK_SIZE : (uint32_t)left;

        memset(st->scratch, 0, JEXFS3_BLOCK_SIZE);
        if (chunk != 0)
            memcpy(st->scratch, data + offset, chunk);
        rc = write_block(st, start + i, st->scratch);
        if (rc != 0)
            return rc;
    }
    return flush_device(st);
}

static int commit_metadata(struct write_state *st)
{
    uint32_t targets[JEXFS3_JOURNAL_MAX_TXN];
    const uint8_t *data[JEXFS3_JOURNAL_MAX_TXN];
    struct jexfs3_journal_io jio;
    uint32_t count = 0;
    uint32_t i;
    int rc;

    jio.read_block = st->io->read_block;
    jio.write_block = st->io->write_block;
    jio.flush = st->io->flush;
    jio.ctx = st->io->ctx;

    for (i = 0; i < st->bitmap_count; i++) {
        targets[count] = st->bitmap[i].block;
        data[count] = st->bitmap[i].image;
        count++;
    }
    targets[count] = st->inode_block;
    data[count] = st->inode_image;
    count++;

    rc = jexfs3_journal_write(st->sb, &jio, targets, data, count);
    if (rc != 0)
        return rc;
    return jexfs3_journal_replay(st->sb, &jio, NULL);
}

static void release_state(struct write_state *st)
{
    uint32_t i;

    for (i = 0; i < st->bitmap_count; i++)
        st->io->free(st->bitmap[i].image);
    if (st->scratch)
        st->io->free(st->scratch);
    if (st->leaf)
        st->io->free(st->leaf);
    if (st->inode_image)
        st->io->free(st->inode_image);
}

int jexfs3_image_replace_file(const struct jexfs3_superblock *sb,
                        const struct jexfs3_write_io *io,
                        uint32_t inode_index,
                        const void *data, uint64_t size)
{
    struct write_state st;
    struct jexfs3_inode old;
    uint64_t inode_byte;
    uint32_t new_blocks;
    uint32_t new_start = 0;
    uint32_t crc;
    int rc;

    if (!sb || !io || !io->read_block || !io->write_block ||
        !io->alloc || !io->free)
        return -EINVAL;
    if (!data && size != 0)
        return -EINVAL;
    if (sb->journal_blocks < JEXFS3_JOURNAL_MIN_BLOCKS)
        return -ENOSYS;
    if (inode_index >= sb->inode_count)
        return -EINVAL;
    if (sb->inode_table_blocks == 0 || sb->block_bitmap_blocks == 0)
        return -EINVAL;
    if (size > (uint64_t)sb->total_blocks * JEXFS3_BLOCK_SIZE)
        return -EFBIG;

    new_blocks = (uint32_t)blocks_for_size(size);
    if (new_blocks > JEXFS3_INLINE_EXTENTS * MAX_EXTENT_BLOCKS)
        return -EFBIG;

    memset(&st, 0, sizeof(st));
    st.sb = sb;
    st.io = io;

    st.scratch = (uint8_t *)io->alloc(JEXFS3_BLOCK_SIZE);
    st.leaf = (uint8_t *)io->alloc(JEXFS3_BLOCK_SIZE);
    st.inode_image = (uint8_t *)io->alloc(JEXFS3_BLOCK_SIZE);
    if (!st.scratch || !st.leaf || !st.inode_image) {
        rc = -ENOMEM;
        goto out;
    }

    inode_byte = (uint64_t)inode_index * JEXFS3_INODE_SIZE;
    st.inode_block = sb->inode_table_start +
                     (uint32_t)(inode_byte >> JEXFS3_BLOCK_SHIFT);
    st.inode_offset = (uint32_t)(inode_byte & JEXFS3_BLOCK_MASK);
    rc = read_block(&st, st.inode_block, st.inode_image);
    if (rc != 0)
        goto out;

    memcpy(&old, st.inode_image + st.inode_offset, sizeof(old));
    if (jexfs3_crc_zeroed(&old, JEXFS3_INODE_SIZE,
                          JEXFS3_INODE_CHECKSUM_OFF) != old.checksum) {
        rc = -EINVAL;
        goto out;
    }
    if ((old.mode & JEXFS3_TYPE_MASK) == JEXFS3_TYPE_DIR) {
        rc = -EISDIR;
        goto out;
    }
    if ((old.mode & JEXFS3_TYPE_MASK) != JEXFS3_TYPE_FILE) {
        rc = -EINVAL;
        goto out;
    }

    rc = load_inode_extents(&st, &old);
    if (rc != 0)
        goto out;
    rc = check_old_blocks_used(&st, &old);
    if (rc != 0)
        goto out;

    rc = alloc_run(&st, new_blocks, &new_start);
    if (rc != 0)
        goto out;
    rc = claim_new_blocks(&st, new_start, new_blocks);
    if (rc != 0)
        goto out;
    rc = release_old_blocks(&st, &old);
    if (rc != 0)
        goto out;

    if (st.bitmap_count + 1u > sb->journal_blocks - 3u) {
        rc = -ENOSPC;
        goto out;
    }

    crc = data_crc((const uint8_t *)data, size);
    rc = build_inode_image(&st, &old, size, new_start, new_blocks, crc);
    if (rc != 0)
        goto out;

    rc = write_data(&st, (const uint8_t *)data, size, new_start, new_blocks);
    if (rc != 0)
        goto out;
    rc = commit_metadata(&st);

out:
    release_state(&st);
    return rc;
}
