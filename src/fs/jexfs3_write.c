/**
 * @file jexfs3_write.c
 * @brief JexFS v3 writable path -- allocate, order data, journal metadata.
 *
 * Purpose: one implementation of the rules a durable file write must
 * follow.  The kernel, the crash simulator and host tooling all call this
 * file, so a change to allocation or write ordering cannot make them drift.
 *
 * Design:
 *   - New data always goes to freshly allocated blocks.  Blocks still
 *     referenced by the old inode are never overwritten, because a crash
 *     before the journal commit must leave the old file readable.
 *   - Data blocks are written and flushed before the metadata transaction
 *     is logged (the ordered-data rule).
 *   - Every metadata block an operation changes is staged in memory and
 *     committed together, so the allocation, the inode records and the
 *     directory entry become visible atomically.
 *   - The replacement is contiguous and uses inline extents only, split
 *     into 65535-block chunks.  A file large enough to need more than four
 *     inline extents is rejected rather than writing an extent leaf.
 *   - Directories change in place inside one staged block.  A removed
 *     entry becomes a free dirent (inode 0) instead of moving bytes, so
 *     unlink is a single-block edit.
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

struct staged_block {
    uint32_t block;
    uint8_t *image;
};

/* Where a name lives, or where a new directory entry would fit. */
struct dir_slot {
    uint32_t entry_block;   /* physical block holding the matching entry */
    uint32_t entry_off;     /* byte offset of the matching entry */
    uint32_t found_inode;   /* 0 when the name is absent */
    uint32_t split_block;   /* physical block with room for a new entry */
    uint32_t split_off;
    uint32_t split_entry_off; /* entry to shorten; used when split_used != 0 */
    uint32_t split_used;    /* bytes the shortened entry keeps; 0 = tombstone */
    uint16_t split_len;
    int has_slot;
};

struct write_state {
    const struct jexfs3_superblock *sb;
    const struct jexfs3_write_io *io;
    struct staged_block staged[JEXFS3_JOURNAL_MAX_TXN];
    uint32_t staged_count;
    uint8_t *scratch;       /* bitmap scans and data staging */
    uint8_t *leaf;          /* extent leaf of the inode being worked on */
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

/*
 * stage_block - load a metadata block into the transaction.
 *
 * Staging is cached by block number, so an inode-table block shared by
 * two inode records is written once.  Every staged block becomes one
 * journal data block.
 */
static int stage_block(struct write_state *st, uint32_t block,
                       uint8_t **out)
{
    uint8_t *image;
    uint32_t i;
    int rc;

    for (i = 0; i < st->staged_count; i++) {
        if (st->staged[i].block == block) {
            *out = st->staged[i].image;
            return 0;
        }
    }
    if (st->staged_count >= JEXFS3_JOURNAL_MAX_TXN)
        return -ENOSPC;

    image = (uint8_t *)st->io->alloc(JEXFS3_BLOCK_SIZE);
    if (!image)
        return -ENOMEM;
    rc = read_block(st, block, image);
    if (rc != 0) {
        st->io->free(image);
        return rc;
    }
    st->staged[st->staged_count].block = block;
    st->staged[st->staged_count].image = image;
    st->staged_count++;
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

    rc = stage_block(st, bitmap_block_of(st->sb, block), &image);
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

/*
 * inode_bitmap_stage - flip one inode bit in the staged bitmap.
 *
 * Mirrors bitmap_stage_change: the caller states the new state, and a
 * bitmap that already agrees is treated as corruption rather than
 * silently accepted.
 */
static int inode_bitmap_stage(struct write_state *st, uint32_t index,
                              int new_used)
{
    const struct jexfs3_superblock *sb = st->sb;
    uint32_t map_block = sb->inode_bitmap_start + index / BITMAP_BITS;
    uint32_t bit = index % BITMAP_BITS;
    uint8_t *image;
    uint32_t byte = bit / 8u;
    uint32_t mask = 1u << (bit % 8u);
    int current;
    int rc;

    rc = stage_block(st, map_block, &image);
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

/* Find the lowest inode number that is free and outside the reserved range. */
static int inode_bitmap_first_free(struct write_state *st, uint32_t *index)
{
    const struct jexfs3_superblock *sb = st->sb;
    uint32_t map_block = UINT32_MAX;

    for (uint32_t i = JEXFS3_ROOT_INODE + 1u; i < sb->inode_count; i++) {
        uint32_t mb = sb->inode_bitmap_start + i / BITMAP_BITS;
        uint32_t bit = i % BITMAP_BITS;
        int used;
        int rc;

        if (mb != map_block) {
            rc = read_block(st, mb, st->scratch);
            if (rc != 0)
                return rc;
            map_block = mb;
        }
        used = (st->scratch[bit / 8u] >> (bit % 8u)) & 1u;
        if (!used) {
            *index = i;
            return 0;
        }
    }
    return -ENOSPC;
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
                              const struct jexfs3_inode *inode,
                              uint8_t *leaf)
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
        rc = read_block(st, inode->extent_root, leaf);
        if (rc != 0)
            return rc;
        eb = (const struct jexfs3_extent_block *)leaf;
        if (eb->magic != JEXFS3_EXTENT_MAGIC || eb->depth != 1 ||
            eb->count == 0 || eb->count > JEXFS3_EXTENT_ENTRIES)
            return -EINVAL;
        crc = jexfs3_crc_zeroed(leaf, JEXFS3_BLOCK_SIZE,
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
                                 const struct jexfs3_inode *inode,
                                 const uint8_t *leaf)
{
    uint32_t i;

    if (inode->extent_depth == 1) {
        const struct jexfs3_extent_block *eb =
            (const struct jexfs3_extent_block *)leaf;

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
                              const struct jexfs3_inode *inode,
                              const uint8_t *leaf)
{
    uint32_t i;
    int rc;

    if (inode->extent_depth == 1) {
        const struct jexfs3_extent_block *eb =
            (const struct jexfs3_extent_block *)leaf;

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
 * stage_inode_record - stage the inode-table block and return its record.
 *
 * The block is validated by the caller after this returns; the pointer
 * stays valid until the transaction is committed.
 */
static int stage_inode_record(struct write_state *st, uint32_t inode_index,
                              struct jexfs3_inode **record)
{
    uint64_t byte_off = (uint64_t)inode_index * JEXFS3_INODE_SIZE;
    uint32_t block = st->sb->inode_table_start +
                     (uint32_t)(byte_off >> JEXFS3_BLOCK_SHIFT);
    uint32_t within = (uint32_t)(byte_off & JEXFS3_BLOCK_MASK);
    uint8_t *image;
    int rc;

    rc = stage_block(st, block, &image);
    if (rc != 0)
        return rc;
    *record = (struct jexfs3_inode *)(image + within);
    return 0;
}

/*
 * rewrite_inode_record - swap in new file contents inside an inode record.
 *
 * The record is copied forward so mode, links and ownership survive.
 * The checksum is recomputed over the modified 256-byte record.
 */
static int rewrite_inode_record(struct jexfs3_inode *record,
                                const struct jexfs3_inode *old,
                                uint64_t size, uint32_t start, uint32_t count,
                                uint32_t crc)
{
    struct jexfs3_inode next;
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

/*
 * Directory helpers.
 *
 * A directory is an extent-mapped block sequence whose bytes are a chain
 * of dirents.  A dirent with inode 0 is free space: the chain still walks
 * over it, but it names nothing.  Unlinking therefore leaves a tombstone
 * instead of shifting entries or shrinking the directory, which keeps the
 * removal to one block image and one journal transaction.
 */

static uint32_t dirent_used_len(const struct jexfs3_dirent *de)
{
    return (JEXFS3_DIRENT_MIN + (uint32_t)de->name_len + 3u) & ~3u;
}

static uint32_t inode_data_blocks(const struct jexfs3_inode *inode)
{
    return (uint32_t)((inode->size + JEXFS3_BLOCK_SIZE - 1u) >>
                      JEXFS3_BLOCK_SHIFT);
}

/*
 * inode_map - translate a file block to a physical block.
 *
 * A depth-1 inode is mapped through st->leaf, which the caller must have
 * filled with load_inode_extents() first.  Keeping one leaf buffer means
 * directory operations must finish with one inode before loading another.
 */
static int inode_map(struct write_state *st,
                     const struct jexfs3_inode *inode, uint32_t logical,
                     uint32_t *physical)
{
    uint32_t i;

    if (inode->extent_depth == 0) {
        for (i = 0; i < inode->extent_count; i++) {
            const struct jexfs3_extent *ext = &inode->inline_extents[i];

            if (logical >= ext->logical &&
                (uint64_t)logical < (uint64_t)ext->logical + ext->length) {
                *physical = ext->physical + (logical - ext->logical);
                return 0;
            }
        }
        return -EINVAL;
    }

    {
        const struct jexfs3_extent_block *eb =
            (const struct jexfs3_extent_block *)st->leaf;

        for (i = 0; i < eb->count; i++) {
            const struct jexfs3_extent *ext = &eb->entries[i];

            if (logical >= ext->logical &&
                (uint64_t)logical < (uint64_t)ext->logical + ext->length) {
                *physical = ext->physical + (logical - ext->logical);
                return 0;
            }
        }
    }
    return -EINVAL;
}

/*
 * dir_scan - find a name and a place for a new entry.
 *
 * The whole directory is walked once, so a duplicate name and a free slot
 * are discovered from the same on-disk state.  split_used == 0 marks a
 * tombstone that can be overwritten whole; otherwise the existing entry
 * at split_entry_off is shortened and the remainder becomes the new entry.
 */
static int dir_scan(struct write_state *st,
                    const struct jexfs3_inode *dir, const char *name,
                    uint32_t need, struct dir_slot *slot)
{
    uint32_t name_len = (uint32_t)strlen(name);
    uint32_t blocks = inode_data_blocks(dir);

    memset(slot, 0, sizeof(*slot));
    for (uint32_t logical = 0; logical < blocks; logical++) {
        uint64_t block_start = (uint64_t)logical << JEXFS3_BLOCK_SHIFT;
        uint32_t limit = JEXFS3_BLOCK_SIZE;
        uint32_t physical;
        uint32_t off = 0;
        int rc;

        rc = inode_map(st, dir, logical, &physical);
        if (rc != 0)
            return rc;
        rc = read_block(st, physical, st->scratch);
        if (rc != 0)
            return rc;
        if (dir->size - block_start < limit)
            limit = (uint32_t)(dir->size - block_start);

        while (off + JEXFS3_DIRENT_MIN <= limit) {
            const struct jexfs3_dirent *de =
                (const struct jexfs3_dirent *)(st->scratch + off);
            uint32_t used;

            if (de->rec_len < JEXFS3_DIRENT_MIN ||
                off + de->rec_len > limit)
                return -EINVAL;

            if (de->inode == 0) {
                if (de->name_len != 0 || de->file_type != 0)
                    return -EINVAL;
                if (!slot->has_slot && de->rec_len >= need) {
                    slot->split_block = physical;
                    slot->split_off = off;
                    slot->split_len = de->rec_len;
                    slot->split_used = 0;
                    slot->has_slot = 1;
                }
            } else {
                if (de->name_len == 0)
                    return -EINVAL;
                used = dirent_used_len(de);
                if (used > de->rec_len)
                    return -EINVAL;
                if (de->name_len == name_len &&
                    memcmp(de->name, name, name_len) == 0) {
                    slot->found_inode = de->inode;
                    slot->entry_block = physical;
                    slot->entry_off = off;
                }
                if (!slot->has_slot && de->rec_len - used >= need) {
                    slot->split_block = physical;
                    slot->split_off = off + used;
                    slot->split_len = (uint16_t)(de->rec_len - used);
                    slot->split_entry_off = off;
                    slot->split_used = used;
                    slot->has_slot = 1;
                }
            }
            off += de->rec_len;
        }
        if (off != limit)
            return -EINVAL;
    }
    return 0;
}

static int dir_add_entry(struct write_state *st, const struct dir_slot *slot,
                         uint32_t inode, const char *name)
{
    uint32_t name_len = (uint32_t)strlen(name);
    struct jexfs3_dirent *de;
    uint8_t *image;
    int rc;

    rc = stage_block(st, slot->split_block, &image);
    if (rc != 0)
        return rc;

    if (slot->split_used != 0) {
        struct jexfs3_dirent *prev =
            (struct jexfs3_dirent *)(image + slot->split_entry_off);

        if (prev->rec_len != slot->split_used + slot->split_len)
            return -EINVAL;
        prev->rec_len = (uint16_t)slot->split_used;
    }

    de = (struct jexfs3_dirent *)(image + slot->split_off);
    memset(de, 0, slot->split_len);
    de->inode = inode;
    de->rec_len = slot->split_len;
    de->name_len = (uint8_t)name_len;
    de->file_type = JEXFS3_DIRENT_FILE;
    memcpy(de->name, name, name_len);
    return 0;
}

static int dir_remove_entry(struct write_state *st, const struct dir_slot *slot)
{
    struct jexfs3_dirent *de;
    uint8_t *image;
    int rc;

    rc = stage_block(st, slot->entry_block, &image);
    if (rc != 0)
        return rc;
    de = (struct jexfs3_dirent *)(image + slot->entry_off);
    if (de->inode == 0 || de->rec_len < JEXFS3_DIRENT_MIN)
        return -EINVAL;
    memset(de->name, 0, de->rec_len - JEXFS3_DIRENT_MIN);
    de->inode = 0;
    de->name_len = 0;
    de->file_type = 0;
    return 0;
}

/*
 * dir_crc - crc over the directory's full data, with one staged block.
 *
 * The edited block is not on disk yet, so it is substituted while the
 * remaining blocks are read normally.  The inode's data_crc covers the
 * whole directory, padding included.
 */
static int dir_crc(struct write_state *st, const struct jexfs3_inode *dir,
                   uint32_t edited_block, const uint8_t *edited,
                   uint32_t *crc_out)
{
    uint32_t blocks = inode_data_blocks(dir);
    uint32_t crc = 0;

    for (uint32_t logical = 0; logical < blocks; logical++) {
        uint32_t physical;
        int rc;

        rc = inode_map(st, dir, logical, &physical);
        if (rc != 0)
            return rc;
        if (physical == edited_block) {
            crc = jexfs3_crc32c(crc, edited, JEXFS3_BLOCK_SIZE);
        } else {
            rc = read_block(st, physical, st->scratch);
            if (rc != 0)
                return rc;
            crc = jexfs3_crc32c(crc, st->scratch, JEXFS3_BLOCK_SIZE);
        }
    }
    *crc_out = crc;
    return 0;
}

/* Update a directory's data crc and generation in its staged record. */
static int update_dir_record(struct write_state *st, uint32_t dir_inode,
                             const struct jexfs3_inode *dir,
                             uint32_t edited_block, const uint8_t *edited)
{
    struct jexfs3_inode *record;
    uint32_t crc;
    int rc;

    rc = dir_crc(st, dir, edited_block, edited, &crc);
    if (rc != 0)
        return rc;
    rc = stage_inode_record(st, dir_inode, &record);
    if (rc != 0)
        return rc;
    if (jexfs3_crc_zeroed(record, JEXFS3_INODE_SIZE,
                          JEXFS3_INODE_CHECKSUM_OFF) != record->checksum)
        return -EINVAL;
    record->data_crc = crc;
    record->generation += 1u;
    record->checksum = jexfs3_crc_zeroed(record, JEXFS3_INODE_SIZE,
                                         JEXFS3_INODE_CHECKSUM_OFF);
    return 0;
}

/* Load a directory inode, checking that it is allocated and a directory. */
static int load_dir_inode(struct write_state *st, uint32_t index,
                          struct jexfs3_inode *out)
{
    uint64_t byte_off = (uint64_t)index * JEXFS3_INODE_SIZE;
    uint32_t block = st->sb->inode_table_start +
                     (uint32_t)(byte_off >> JEXFS3_BLOCK_SHIFT);
    uint32_t within = (uint32_t)(byte_off & JEXFS3_BLOCK_MASK);
    uint32_t map_block = st->sb->inode_bitmap_start +
                         index / BITMAP_BITS;
    uint32_t bit = index % BITMAP_BITS;
    int used;
    int rc;

    if (index >= st->sb->inode_count)
        return -EINVAL;
    rc = read_block(st, map_block, st->scratch);
    if (rc != 0)
        return rc;
    used = (st->scratch[bit / 8u] >> (bit % 8u)) & 1u;
    if (!used)
        return -ENOENT;

    rc = read_block(st, block, st->scratch);
    if (rc != 0)
        return rc;
    memcpy(out, st->scratch + within, JEXFS3_INODE_SIZE);
    if (jexfs3_crc_zeroed(out, JEXFS3_INODE_SIZE,
                          JEXFS3_INODE_CHECKSUM_OFF) != out->checksum)
        return -EINVAL;
    if ((out->mode & JEXFS3_TYPE_MASK) != JEXFS3_TYPE_DIR)
        return -ENOTDIR;
    return 0;
}

/* Load a regular file inode without staging its record. */
static int load_file_inode(struct write_state *st, uint32_t index,
                           struct jexfs3_inode *out)
{
    uint64_t byte_off = (uint64_t)index * JEXFS3_INODE_SIZE;
    uint32_t block = st->sb->inode_table_start +
                     (uint32_t)(byte_off >> JEXFS3_BLOCK_SHIFT);
    uint32_t within = (uint32_t)(byte_off & JEXFS3_BLOCK_MASK);
    uint32_t map_block = st->sb->inode_bitmap_start + index / BITMAP_BITS;
    uint32_t bit = index % BITMAP_BITS;
    int used;
    int rc;

    if (index >= st->sb->inode_count)
        return -EINVAL;
    rc = read_block(st, map_block, st->scratch);
    if (rc != 0)
        return rc;
    used = (st->scratch[bit / 8u] >> (bit % 8u)) & 1u;
    if (!used)
        return -EINVAL;

    rc = read_block(st, block, st->scratch);
    if (rc != 0)
        return rc;
    memcpy(out, st->scratch + within, JEXFS3_INODE_SIZE);
    if (jexfs3_crc_zeroed(out, JEXFS3_INODE_SIZE,
                          JEXFS3_INODE_CHECKSUM_OFF) != out->checksum)
        return -EINVAL;
    if ((out->mode & JEXFS3_TYPE_MASK) == JEXFS3_TYPE_DIR)
        return -EISDIR;
    if ((out->mode & JEXFS3_TYPE_MASK) != JEXFS3_TYPE_FILE)
        return -EINVAL;
    return 0;
}

static int check_name(const char *name)
{
    size_t len;

    if (!name)
        return -EINVAL;
    len = strlen(name);
    if (len == 0 || len > JEXFS3_NAME_MAX)
        return -ENAMETOOLONG;
    if (strchr(name, '/'))
        return -EINVAL;
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
        return -EINVAL;
    return 0;
}

static int commit_metadata(struct write_state *st)
{
    uint32_t targets[JEXFS3_JOURNAL_MAX_TXN];
    const uint8_t *data[JEXFS3_JOURNAL_MAX_TXN];
    struct jexfs3_journal_io jio;
    uint32_t i;
    int rc;

    jio.read_block = st->io->read_block;
    jio.write_block = st->io->write_block;
    jio.flush = st->io->flush;
    jio.ctx = st->io->ctx;

    for (i = 0; i < st->staged_count; i++) {
        targets[i] = st->staged[i].block;
        data[i] = st->staged[i].image;
    }

    rc = jexfs3_journal_write(st->sb, &jio, targets, data,
                              st->staged_count);
    if (rc != 0)
        return rc;
    return jexfs3_journal_replay(st->sb, &jio, NULL);
}

static void release_state(struct write_state *st)
{
    uint32_t i;

    for (i = 0; i < st->staged_count; i++)
        st->io->free(st->staged[i].image);
    if (st->scratch)
        st->io->free(st->scratch);
    if (st->leaf)
        st->io->free(st->leaf);
}

int jexfs3_image_replace_file(const struct jexfs3_superblock *sb,
                        const struct jexfs3_write_io *io,
                        uint32_t inode_index,
                        const void *data, uint64_t size)
{
    struct write_state st;
    struct jexfs3_inode old;
    struct jexfs3_inode *record;
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
    if (!st.scratch || !st.leaf) {
        rc = -ENOMEM;
        goto out;
    }

    rc = stage_inode_record(&st, inode_index, &record);
    if (rc != 0)
        goto out;
    memcpy(&old, record, sizeof(old));
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

    rc = load_inode_extents(&st, &old, st.leaf);
    if (rc != 0)
        goto out;
    rc = check_old_blocks_used(&st, &old, st.leaf);
    if (rc != 0)
        goto out;

    rc = alloc_run(&st, new_blocks, &new_start);
    if (rc != 0)
        goto out;
    rc = claim_new_blocks(&st, new_start, new_blocks);
    if (rc != 0)
        goto out;
    rc = release_old_blocks(&st, &old, st.leaf);
    if (rc != 0)
        goto out;

    if (st.staged_count > sb->journal_blocks - 3u) {
        rc = -ENOSPC;
        goto out;
    }

    crc = data_crc((const uint8_t *)data, size);
    rc = rewrite_inode_record(record, &old, size, new_start, new_blocks, crc);
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

/* Allocate the shared scratch buffers and zero the write state. */
static int init_write_state(struct write_state *st,
                            const struct jexfs3_superblock *sb,
                            const struct jexfs3_write_io *io)
{
    memset(st, 0, sizeof(*st));
    st->sb = sb;
    st->io = io;
    st->scratch = (uint8_t *)io->alloc(JEXFS3_BLOCK_SIZE);
    st->leaf = (uint8_t *)io->alloc(JEXFS3_BLOCK_SIZE);
    if (!st->scratch || !st->leaf)
        return -ENOMEM;
    return 0;
}

int jexfs3_image_create_file(const struct jexfs3_superblock *sb,
                             const struct jexfs3_write_io *io,
                             uint32_t parent_inode, const char *name,
                             uint32_t *inode_out)
{
    struct write_state st;
    struct jexfs3_inode dir;
    struct jexfs3_inode *record;
    struct dir_slot slot;
    uint8_t *dir_image;
    uint32_t need;
    uint32_t new_inode;
    int rc;

    if (!sb || !io || !io->read_block || !io->write_block ||
        !io->alloc || !io->free)
        return -EINVAL;
    rc = check_name(name);
    if (rc != 0)
        return rc;
    if (sb->journal_blocks < JEXFS3_JOURNAL_MIN_BLOCKS)
        return -ENOSYS;
    if (parent_inode >= sb->inode_count)
        return -EINVAL;
    if (sb->inode_bitmap_blocks == 0 || sb->inode_table_blocks == 0 ||
        sb->block_bitmap_blocks == 0)
        return -EINVAL;

    rc = init_write_state(&st, sb, io);
    if (rc != 0)
        goto out;

    rc = load_dir_inode(&st, parent_inode, &dir);
    if (rc != 0)
        goto out;
    rc = load_inode_extents(&st, &dir, st.leaf);
    if (rc != 0)
        goto out;

    need = (JEXFS3_DIRENT_MIN + (uint32_t)strlen(name) + 3u) & ~3u;
    rc = dir_scan(&st, &dir, name, need, &slot);
    if (rc != 0)
        goto out;
    if (slot.found_inode != 0) {
        rc = -EEXIST;
        goto out;
    }
    if (!slot.has_slot) {
        rc = -ENOSPC;
        goto out;
    }
    rc = inode_bitmap_first_free(&st, &new_inode);
    if (rc != 0)
        goto out;
    rc = inode_bitmap_stage(&st, new_inode, 1);
    if (rc != 0)
        goto out;

    rc = stage_inode_record(&st, new_inode, &record);
    if (rc != 0)
        goto out;
    memset(record, 0, JEXFS3_INODE_SIZE);
    record->mode = (uint16_t)(JEXFS3_TYPE_FILE | JEXFS3_IRUSR |
                              JEXFS3_IWUSR | JEXFS3_IRGRP | JEXFS3_IROTH);
    record->links = 1;
    record->generation = 1;
    record->data_crc = jexfs3_crc32c(0, NULL, 0);
    record->checksum = jexfs3_crc_zeroed(record, JEXFS3_INODE_SIZE,
                                         JEXFS3_INODE_CHECKSUM_OFF);

    rc = dir_add_entry(&st, &slot, new_inode, name);
    if (rc != 0)
        goto out;
    rc = stage_block(&st, slot.split_block, &dir_image);
    if (rc != 0)
        goto out;
    rc = update_dir_record(&st, parent_inode, &dir, slot.split_block,
                           dir_image);
    if (rc != 0)
        goto out;

    if (st.staged_count > sb->journal_blocks - 3u) {
        rc = -ENOSPC;
        goto out;
    }
    rc = commit_metadata(&st);
    if (rc == 0 && inode_out)
        *inode_out = new_inode;

out:
    release_state(&st);
    return rc;
}

int jexfs3_image_unlink_file(const struct jexfs3_superblock *sb,
                             const struct jexfs3_write_io *io,
                             uint32_t parent_inode, const char *name)
{
    struct write_state st;
    struct jexfs3_inode dir;
    struct jexfs3_inode victim;
    struct dir_slot slot;
    uint8_t *dir_image;
    uint32_t need;
    int rc;

    if (!sb || !io || !io->read_block || !io->write_block ||
        !io->alloc || !io->free)
        return -EINVAL;
    rc = check_name(name);
    if (rc != 0)
        return rc;
    if (sb->journal_blocks < JEXFS3_JOURNAL_MIN_BLOCKS)
        return -ENOSYS;
    if (parent_inode >= sb->inode_count)
        return -EINVAL;
    if (sb->inode_bitmap_blocks == 0 || sb->inode_table_blocks == 0 ||
        sb->block_bitmap_blocks == 0)
        return -EINVAL;

    rc = init_write_state(&st, sb, io);
    if (rc != 0)
        goto out;

    rc = load_dir_inode(&st, parent_inode, &dir);
    if (rc != 0)
        goto out;
    rc = load_inode_extents(&st, &dir, st.leaf);
    if (rc != 0)
        goto out;

    need = (JEXFS3_DIRENT_MIN + (uint32_t)strlen(name) + 3u) & ~3u;
    rc = dir_scan(&st, &dir, name, need, &slot);
    if (rc != 0)
        goto out;
    if (slot.found_inode == 0) {
        rc = -ENOENT;
        goto out;
    }

    rc = load_file_inode(&st, slot.found_inode, &victim);
    if (rc != 0)
        goto out;
    if (victim.links != 1) {
        rc = -EINVAL;
        goto out;
    }

    rc = dir_remove_entry(&st, &slot);
    if (rc != 0)
        goto out;
    rc = stage_block(&st, slot.entry_block, &dir_image);
    if (rc != 0)
        goto out;
    rc = update_dir_record(&st, parent_inode, &dir, slot.entry_block,
                           dir_image);
    if (rc != 0)
        goto out;

    /* The parent map is no longer needed, so the leaf buffer is free. */
    rc = load_inode_extents(&st, &victim, st.leaf);
    if (rc != 0)
        goto out;
    rc = check_old_blocks_used(&st, &victim, st.leaf);
    if (rc != 0)
        goto out;
    rc = release_old_blocks(&st, &victim, st.leaf);
    if (rc != 0)
        goto out;
    rc = inode_bitmap_stage(&st, slot.found_inode, 0);
    if (rc != 0)
        goto out;

    if (st.staged_count > sb->journal_blocks - 3u) {
        rc = -ENOSPC;
        goto out;
    }
    rc = commit_metadata(&st);

out:
    release_state(&st);
    return rc;
}
