/**
 * @file jexfs3.c
 * @brief JexFS v3 -- read-only kernel driver (phase 1).
 *
 * The on-disk format is defined in jexfs3.h.  Block access goes through
 * a caller-supplied callback, so the same driver will later serve both
 * a file-backed test image and an IDE partition.  A single 4 KB block
 * cache keeps metadata parsing off the 8 KB kernel stack; a mutex makes
 * that cache safe for concurrent readers.
 *
 * Everything is validated on the way in: superblock and inode crc32c,
 * extent bounds and coverage, directory entry chains, and per-inode
 * data checksums.  Writes, journals, and block allocation belong to
 * later phases and are intentionally absent.
 */

#define pr_fmt(fmt) "[JEXFS3] " fmt

#include "jexfs3_fs.h"
#include "jexfs3_journal.h"
#include "kheap.h"
#include "string.h"
#include "kernel/mutex.h"
#include "kernel/printk.h"
#include <jexos/errno.h>

#define JEXFS3_BLOCK_SHIFT 12u
#define JEXFS3_BLOCK_MASK  (JEXFS3_BLOCK_SIZE - 1u)

static struct jexfs3_superblock fs3_sb;
static struct jexfs3_device fs3_dev;
static int fs3_mounted;
static int fs3_sb_ready;
static int fs3_lock_ready;

/* The cache is shared by every reader; fs3_lock serialises access. */
static uint8_t fs3_cache[JEXFS3_BLOCK_SIZE] __attribute__((aligned(16)));
static uint32_t fs3_cache_block;
static int fs3_cache_valid;

static mutex_t fs3_lock;

/*
 * cache_fill - load one 4 KB block, reusing the cached copy when possible.
 *
 * Before the superblock is validated there is no block count to check,
 * so range enforcement starts at fs3_sb_ready.
 */
static int cache_fill(uint32_t block)
{
    if (fs3_cache_valid && fs3_cache_block == block)
        return 0;
    if (fs3_sb_ready && block >= fs3_sb.total_blocks)
        return -EINVAL;
    if (fs3_dev.read_block(fs3_dev.ctx, block, fs3_cache) != 0)
        return -EIO;
    fs3_cache_block = block;
    fs3_cache_valid = 1;
    return 0;
}

static uint64_t blocks_for_size(uint64_t size)
{
    return (size + JEXFS3_BLOCK_SIZE - 1u) >> JEXFS3_BLOCK_SHIFT;
}

static int extent_range_ok(const struct jexfs3_extent *ext)
{
    uint64_t end;

    if (ext->length == 0 || ext->flags != 0)
        return -EINVAL;
    if (ext->physical < fs3_sb.first_data_block)
        return -EINVAL;
    end = (uint64_t)ext->physical + ext->length;
    if (end > fs3_sb.total_blocks - 1u)
        return -EINVAL;
    return 0;
}

/*
 * extent_block_check - validate an already-loaded leaf extent block.
 *
 * The block sits in fs3_cache; callers parse entries from there without
 * another fill, so the contents cannot change underneath them.
 */
static int extent_block_check(const struct jexfs3_extent_block *eb)
{
    uint32_t crc;

    if (eb->magic != JEXFS3_EXTENT_MAGIC || eb->depth != 1 ||
        eb->count == 0 || eb->count > JEXFS3_EXTENT_ENTRIES)
        return -EINVAL;
    crc = jexfs3_crc_zeroed(eb, JEXFS3_BLOCK_SIZE,
                            JEXFS3_EXTENT_CHECKSUM_OFF);
    if (crc != eb->checksum)
        return -EINVAL;
    return 0;
}

static int extent_block_load(uint32_t block,
                             const struct jexfs3_extent_block **out)
{
    int rc;

    if (block < fs3_sb.first_data_block ||
        block >= fs3_sb.total_blocks - 1u)
        return -EINVAL;
    rc = cache_fill(block);
    if (rc != 0)
        return rc;
    if (extent_block_check((const struct jexfs3_extent_block *)fs3_cache) != 0)
        return -EINVAL;
    *out = (const struct jexfs3_extent_block *)fs3_cache;
    return 0;
}

static int extents_check_nolock(const struct jexfs3_inode *inode,
                                uint32_t *data_blocks)
{
    uint64_t expected64;
    uint32_t expected;
    uint32_t logical = 0;
    uint32_t total = 0;
    uint32_t i;

    if (inode->size > (uint64_t)fs3_sb.total_blocks * JEXFS3_BLOCK_SIZE)
        return -EINVAL;
    expected64 = blocks_for_size(inode->size);

    if (expected64 > fs3_sb.total_blocks)
        return -EINVAL;
    expected = (uint32_t)expected64;

    if (inode->extent_depth > 1)
        return -EINVAL;

    if (inode->extent_depth == 1) {
        const struct jexfs3_extent_block *eb;

        if (extent_block_load(inode->extent_root, &eb) != 0)
            return -EINVAL;
        for (i = 0; i < eb->count; i++) {
            const struct jexfs3_extent *ext = &eb->entries[i];

            if (extent_range_ok(ext) != 0 || ext->logical != logical)
                return -EINVAL;
            logical += ext->length;
            total += ext->length;
        }
    } else {
        if (inode->extent_count > JEXFS3_INLINE_EXTENTS)
            return -EINVAL;
        for (i = 0; i < inode->extent_count; i++) {
            const struct jexfs3_extent *ext = &inode->inline_extents[i];

            if (extent_range_ok(ext) != 0 || ext->logical != logical)
                return -EINVAL;
            logical += ext->length;
            total += ext->length;
        }
    }

    if (total != expected)
        return -EINVAL;
    if (inode->blocks != (uint64_t)total + (inode->extent_depth ? 1u : 0u))
        return -EINVAL;

    *data_blocks = total;
    return 0;
}

static int bmap_nolock(const struct jexfs3_inode *inode, uint32_t logical,
                       uint32_t *physical)
{
    uint32_t i;

    if (inode->extent_depth == 1) {
        const struct jexfs3_extent_block *eb;

        if (extent_block_load(inode->extent_root, &eb) != 0)
            return -EIO;
        for (i = 0; i < eb->count; i++) {
            const struct jexfs3_extent *ext = &eb->entries[i];

            if (logical >= ext->logical &&
                (uint64_t)logical < (uint64_t)ext->logical + ext->length) {
                *physical = ext->physical + (logical - ext->logical);
                return 0;
            }
        }
        return -EIO;
    }

    for (i = 0; i < inode->extent_count; i++) {
        const struct jexfs3_extent *ext = &inode->inline_extents[i];

        if (logical >= ext->logical &&
            (uint64_t)logical < (uint64_t)ext->logical + ext->length) {
            *physical = ext->physical + (logical - ext->logical);
            return 0;
        }
    }
    return -EIO;
}

static int inode_read_nolock(uint32_t index, struct jexfs3_inode *out)
{
    uint64_t byte_off;
    uint32_t block;
    uint32_t within;
    uint32_t crc;
    int rc;

    if (index >= fs3_sb.inode_count)
        return -EINVAL;

    byte_off = (uint64_t)index * JEXFS3_INODE_SIZE;
    block = fs3_sb.inode_table_start + (uint32_t)(byte_off >> JEXFS3_BLOCK_SHIFT);
    within = (uint32_t)(byte_off & JEXFS3_BLOCK_MASK);

    rc = cache_fill(block);
    if (rc != 0)
        return rc;
    memcpy(out, fs3_cache + within, JEXFS3_INODE_SIZE);

    crc = jexfs3_crc_zeroed(out, JEXFS3_INODE_SIZE,
                            JEXFS3_INODE_CHECKSUM_OFF);
    if (crc != out->checksum)
        return -EINVAL;
    return 0;
}

static int inode_used_nolock(uint32_t index)
{
    uint32_t byte = index / 8u;
    uint32_t block = fs3_sb.inode_bitmap_start + (byte >> JEXFS3_BLOCK_SHIFT);
    uint32_t within = byte & JEXFS3_BLOCK_MASK;
    int rc;

    if (index >= fs3_sb.inode_count)
        return -EINVAL;
    rc = cache_fill(block);
    if (rc != 0)
        return rc;
    return (fs3_cache[within] >> (index % 8u)) & 1u;
}

/*
 * read_nolock - copy file data out of the extent tree.
 *
 * Callers must hold fs3_lock.  The inode is passed in so a multi-block
 * read does not re-read and re-checksum it for every chunk.
 */
static int read_nolock(const struct jexfs3_inode *inode, void *buffer,
                       uint32_t size, uint64_t offset)
{
    uint8_t *out = (uint8_t *)buffer;
    uint64_t pos = offset;
    uint32_t left;
    int rc;

    if (offset >= inode->size)
        return 0;
    if ((uint64_t)size > inode->size - offset)
        size = (uint32_t)(inode->size - offset);

    left = size;
    while (left > 0) {
        uint32_t logical = (uint32_t)(pos >> JEXFS3_BLOCK_SHIFT);
        uint32_t within = (uint32_t)(pos & JEXFS3_BLOCK_MASK);
        uint32_t chunk = JEXFS3_BLOCK_SIZE - within;
        uint32_t physical;

        if (chunk > left)
            chunk = left;
        rc = bmap_nolock(inode, logical, &physical);
        if (rc != 0)
            return rc;
        rc = cache_fill(physical);
        if (rc != 0)
            return rc;

        memcpy(out, fs3_cache + within, chunk);
        out += chunk;
        pos += chunk;
        left -= chunk;
    }
    return (int)size;
}

static int readdir_nolock(uint32_t index, uint64_t *offset, char *name,
                          uint32_t name_cap, uint32_t *child_out,
                          uint8_t *type_out)
{
    struct jexfs3_inode inode;
    const struct jexfs3_dirent *de;
    uint64_t pos = *offset;
    uint64_t block_start;
    uint32_t data_blocks;
    uint32_t logical;
    uint32_t within;
    uint32_t limit;
    uint32_t physical;
    int rc;

    rc = inode_read_nolock(index, &inode);
    if (rc != 0)
        return rc;
    if ((inode.mode & JEXFS3_TYPE_MASK) != JEXFS3_TYPE_DIR)
        return -ENOTDIR;
    rc = extents_check_nolock(&inode, &data_blocks);
    if (rc != 0)
        return rc;
    (void)data_blocks;
    if (pos >= inode.size)
        return 0;
    if (name_cap < 2)
        return -EINVAL;

    logical = (uint32_t)(pos >> JEXFS3_BLOCK_SHIFT);
    within = (uint32_t)(pos & JEXFS3_BLOCK_MASK);
    block_start = (uint64_t)logical << JEXFS3_BLOCK_SHIFT;

    rc = bmap_nolock(&inode, logical, &physical);
    if (rc != 0)
        return rc;
    rc = cache_fill(physical);
    if (rc != 0)
        return rc;

    limit = JEXFS3_BLOCK_SIZE;
    if (inode.size - block_start < JEXFS3_BLOCK_SIZE)
        limit = (uint32_t)(inode.size - block_start);
    if (within + JEXFS3_DIRENT_MIN > limit)
        return -EINVAL;

    de = (const struct jexfs3_dirent *)(fs3_cache + within);
    if (de->rec_len < JEXFS3_DIRENT_MIN || within + de->rec_len > limit)
        return -EINVAL;
    if (de->name_len == 0 ||
        de->name_len > de->rec_len - JEXFS3_DIRENT_MIN)
        return -EINVAL;
    if (de->name_len >= name_cap)
        return -ERANGE;
    if (de->inode >= fs3_sb.inode_count ||
        de->inode < JEXFS3_ROOT_INODE)
        return -EINVAL;

    memcpy(name, de->name, de->name_len);
    name[de->name_len] = '\0';
    *child_out = de->inode;
    *type_out = de->file_type;
    *offset = pos + de->rec_len;
    return 1;
}

static int superblock_check(const struct jexfs3_superblock *sb)
{
    uint64_t inode_bitmap_blocks;
    uint64_t block_bitmap_blocks;
    uint64_t inode_table_blocks;
    uint64_t block_bitmap_start;
    uint64_t inode_table_start;
    uint64_t journal_start;
    uint64_t first_data_block;

    if (sb->magic != JEXFS3_MAGIC || sb->version != JEXFS3_VERSION)
        return -EINVAL;
    if (sb->block_size != JEXFS3_BLOCK_SIZE ||
        sb->inode_size != JEXFS3_INODE_SIZE)
        return -EINVAL;
    if (sb->total_blocks < 32)
        return -EINVAL;
    if (sb->total_bytes !=
        (uint64_t)sb->total_blocks * JEXFS3_BLOCK_SIZE)
        return -EINVAL;
    if (fs3_dev.media_bytes != 0 && sb->total_bytes != fs3_dev.media_bytes)
        return -EINVAL;
    if (sb->checksum_type != 1)
        return -EINVAL;
    if ((sb->feature_incompat & ~JEXFS3_FEATURE_INCOMPAT_KNOWN) != 0)
        return -EINVAL;
    if ((sb->feature_incompat & JEXFS3_FEATURE_INCOMPAT_EXTENTS) == 0 ||
        (sb->feature_incompat & JEXFS3_FEATURE_INCOMPAT_CRC32C) == 0)
        return -EINVAL;
    if (sb->journal_blocks == 0) {
        /* A journal bit with no region, or a region with no bit, means
         * the superblock and the layout disagree. */
        if (sb->journal_start != 0 ||
            (sb->feature_incompat & JEXFS3_FEATURE_INCOMPAT_JOURNAL))
            return -EINVAL;
    } else {
        if ((sb->feature_incompat & JEXFS3_FEATURE_INCOMPAT_JOURNAL) == 0)
            return -EINVAL;
        if (sb->journal_blocks < JEXFS3_JOURNAL_MIN_BLOCKS)
            return -EINVAL;
    }

    /* Compute the layout in 64 bits: a field that does not fit a 32-bit
     * block number must be rejected, not truncated. */
    inode_bitmap_blocks = ((uint64_t)sb->inode_count + 32767u) / 32768u;
    block_bitmap_blocks = ((uint64_t)sb->total_blocks + 32767u) / 32768u;
    inode_table_blocks = ((uint64_t)sb->inode_count * JEXFS3_INODE_SIZE +
                          JEXFS3_BLOCK_SIZE - 1u) / JEXFS3_BLOCK_SIZE;
    if (inode_bitmap_blocks == 0) inode_bitmap_blocks = 1;
    if (block_bitmap_blocks == 0) block_bitmap_blocks = 1;
    if (inode_table_blocks == 0) inode_table_blocks = 1;

    block_bitmap_start = 1 + inode_bitmap_blocks;
    inode_table_start = block_bitmap_start + block_bitmap_blocks;
    journal_start = inode_table_start + inode_table_blocks;
    first_data_block = journal_start + sb->journal_blocks;
    if (inode_bitmap_blocks > UINT32_MAX ||
        block_bitmap_blocks > UINT32_MAX ||
        inode_table_blocks > UINT32_MAX ||
        block_bitmap_start > UINT32_MAX ||
        inode_table_start > UINT32_MAX ||
        journal_start > UINT32_MAX ||
        first_data_block > UINT32_MAX)
        return -EINVAL;

    if (sb->inode_bitmap_start != 1 ||
        sb->inode_bitmap_blocks != inode_bitmap_blocks ||
        sb->block_bitmap_start != block_bitmap_start ||
        sb->block_bitmap_blocks != block_bitmap_blocks ||
        sb->inode_table_start != inode_table_start ||
        sb->inode_table_blocks != inode_table_blocks ||
        sb->journal_start != (sb->journal_blocks ? journal_start : 0) ||
        sb->first_data_block != first_data_block)
        return -EINVAL;
    if (sb->backup_block != sb->total_blocks - 1)
        return -EINVAL;
    if (first_data_block >= sb->backup_block)
        return -EINVAL;
    if (journal_start >= sb->backup_block)
        return -EINVAL;
    if (sb->root_inode != JEXFS3_ROOT_INODE ||
        sb->root_inode >= sb->inode_count)
        return -EINVAL;
    return 0;
}

int jexfs3_mount(const struct jexfs3_device *dev)
{
    struct jexfs3_superblock backup;
    struct jexfs3_inode root;
    const char *stage = "device";
    uint32_t data_blocks;
    int rc;

    if (!dev || !dev->read_block)
        return -EINVAL;
    if (fs3_mounted)
        return -EBUSY;
    if (!fs3_lock_ready) {
        mutex_init(&fs3_lock, "jexfs3");
        fs3_lock_ready = 1;
    }

    fs3_dev = *dev;
    fs3_cache_valid = 0;
    fs3_sb_ready = 0;

    rc = cache_fill(0);
    if (rc != 0)
        goto fail;
    memcpy(&fs3_sb, fs3_cache, sizeof(fs3_sb));

    stage = "superblock checksum";
    if (jexfs3_crc_zeroed(fs3_cache, JEXFS3_BLOCK_SIZE,
                          JEXFS3_SB_CHECKSUM_OFF) != fs3_sb.checksum) {
        rc = -EINVAL;
        goto fail;
    }

    stage = "superblock";
    rc = superblock_check(&fs3_sb);
    if (rc != 0)
        goto fail;
    fs3_sb_ready = 1;

    if (fs3_sb.journal_blocks != 0) {
        struct jexfs3_journal_io jio;
        struct jexfs3_journal_info jinfo;

        jio.read_block = fs3_dev.read_block;
        jio.write_block = fs3_dev.write_block;
        jio.flush = fs3_dev.flush;
        jio.ctx = fs3_dev.ctx;

        stage = "journal";
        rc = jexfs3_journal_check(&fs3_sb, &jio, &jinfo);
        if (rc != 0)
            goto fail;

        stage = "journal replay";
        rc = jexfs3_journal_replay(&fs3_sb, &jio, &jinfo);
        if (rc != 0)
            goto fail;
        if (jinfo.replayed)
            pr_info("journal: replayed %u metadata block(s)\n",
                    jinfo.txn_count);

        /* A transaction may have rewritten the superblock itself, so the
         * in-memory copy has to be reloaded before anything else uses it. */
        if (jinfo.replayed) {
            stage = "superblock after replay";
            fs3_cache_valid = 0;
            rc = cache_fill(0);
            if (rc != 0)
                goto fail;
            memcpy(&fs3_sb, fs3_cache, sizeof(fs3_sb));
            if (jexfs3_crc_zeroed(fs3_cache, JEXFS3_BLOCK_SIZE,
                                  JEXFS3_SB_CHECKSUM_OFF) != fs3_sb.checksum) {
                rc = -EINVAL;
                goto fail;
            }
            rc = superblock_check(&fs3_sb);
            if (rc != 0)
                goto fail;
        }
    }

    stage = "backup superblock";
    rc = cache_fill(fs3_sb.backup_block);
    if (rc != 0)
        goto fail;
    memcpy(&backup, fs3_cache, sizeof(backup));
    if (jexfs3_crc_zeroed(fs3_cache, JEXFS3_BLOCK_SIZE,
                          JEXFS3_SB_CHECKSUM_OFF) != backup.checksum) {
        rc = -EINVAL;
        goto fail;
    }
    if (memcmp(&backup, &fs3_sb, sizeof(backup)) != 0) {
        rc = -EINVAL;
        goto fail;
    }

    stage = "root inode";
    rc = inode_read_nolock(fs3_sb.root_inode, &root);
    if (rc != 0)
        goto fail;
    if ((root.mode & JEXFS3_TYPE_MASK) != JEXFS3_TYPE_DIR) {
        rc = -EINVAL;
        goto fail;
    }
    stage = "root extents";
    rc = extents_check_nolock(&root, &data_blocks);
    if (rc != 0)
        goto fail;

    fs3_mounted = 1;
    return 0;

fail:
    pr_err("mount failed at %s: %d\n", stage, rc);
    fs3_cache_valid = 0;
    fs3_sb_ready = 0;
    memset(&fs3_dev, 0, sizeof(fs3_dev));
    return rc;
}

void jexfs3_unmount(void)
{
    if (!fs3_mounted)
        return;
    mutex_lock(&fs3_lock);
    fs3_mounted = 0;
    fs3_cache_valid = 0;
    fs3_sb_ready = 0;
    memset(&fs3_dev, 0, sizeof(fs3_dev));
    mutex_unlock(&fs3_lock);
}

int jexfs3_mounted(void)
{
    return fs3_mounted;
}

int jexfs3_superblock(struct jexfs3_superblock *out)
{
    int rc;

    if (!out)
        return -EINVAL;
    if (!fs3_mounted)
        return -ENODEV;
    mutex_lock(&fs3_lock);
    memcpy(out, &fs3_sb, sizeof(*out));
    rc = 0;
    mutex_unlock(&fs3_lock);
    return rc;
}

int jexfs3_lookup(const char *path, uint32_t *inode_out)
{
    char name[JEXFS3_NAME_MAX + 1];
    uint32_t current;
    const char *p;
    int rc;

    if (!path || !inode_out)
        return -EINVAL;
    if (!fs3_mounted)
        return -ENODEV;
    if (strlen(path) > 4096)
        return -ENAMETOOLONG;

    mutex_lock(&fs3_lock);

    current = fs3_sb.root_inode;
    p = path;
    while (*p == '/')
        p++;

    if (*p == '\0') {
        *inode_out = current;
        rc = 0;
        goto out;
    }

    while (*p) {
        uint32_t len = 0;
        uint64_t cookie = 0;
        uint32_t child = 0;
        uint8_t type = 0;
        int found = 0;

        while (*p && *p != '/') {
            if (len >= JEXFS3_NAME_MAX) {
                rc = -ENAMETOOLONG;
                goto out;
            }
            name[len++] = *p++;
        }
        name[len] = '\0';
        while (*p == '/')
            p++;

        for (;;) {
            char entry[JEXFS3_NAME_MAX + 1];

            rc = readdir_nolock(current, &cookie, entry, sizeof(entry),
                                &child, &type);
            if (rc < 0)
                goto out;
            if (rc == 0)
                break;
            if (strcmp(entry, name) == 0) {
                found = 1;
                break;
            }
        }
        if (!found) {
            rc = -ENOENT;
            goto out;
        }

        if (*p != '\0') {
            struct jexfs3_inode child_inode;

            rc = inode_read_nolock(child, &child_inode);
            if (rc != 0)
                goto out;
            if ((child_inode.mode & JEXFS3_TYPE_MASK) != JEXFS3_TYPE_DIR) {
                rc = -ENOTDIR;
                goto out;
            }
        }
        current = child;
    }

    *inode_out = current;
    rc = 0;

out:
    mutex_unlock(&fs3_lock);
    return rc;
}

int jexfs3_read_file(uint32_t index, void *buf, uint32_t size, uint64_t offset)
{
    struct jexfs3_inode inode;
    uint32_t data_blocks;
    int rc;

    if (!buf && size != 0)
        return -EINVAL;
    if (!fs3_mounted)
        return -ENODEV;

    mutex_lock(&fs3_lock);
    rc = inode_read_nolock(index, &inode);
    if (rc == 0 &&
        (inode.mode & JEXFS3_TYPE_MASK) != JEXFS3_TYPE_FILE)
        rc = -EISDIR;
    if (rc == 0)
        rc = extents_check_nolock(&inode, &data_blocks);
    if (rc == 0)
        rc = read_nolock(&inode, buf, size, offset);
    (void)data_blocks;
    mutex_unlock(&fs3_lock);
    return rc;
}

int jexfs3_readdir(uint32_t index, uint64_t *offset, char *name,
                   uint32_t name_cap, uint32_t *child_out, uint8_t *type_out)
{
    int rc;

    if (!offset || !name || !child_out || !type_out)
        return -EINVAL;
    if (!fs3_mounted)
        return -ENODEV;

    mutex_lock(&fs3_lock);
    rc = readdir_nolock(index, offset, name, name_cap, child_out, type_out);
    mutex_unlock(&fs3_lock);
    return rc;
}

/*
 * verify_dir_nolock - walk a directory's entries and mark reachable inodes.
 *
 * Entry names, child inode numbers and dirent types are checked against
 * the child inode itself.  "." and ".." must be present and point where
 * the format says they do.
 */
static int verify_dir_nolock(uint32_t index, const struct jexfs3_inode *inode,
                             uint8_t *reachable)
{
    uint64_t cookie = 0;
    uint32_t subdirs = 0;
    int has_dot = 0;
    int has_dotdot = 0;
    int rc;

    for (;;) {
        char name[JEXFS3_NAME_MAX + 1];
        struct jexfs3_inode child_inode;
        uint32_t child = 0;
        uint8_t type = 0;
        uint16_t child_type;

        rc = readdir_nolock(index, &cookie, name, sizeof(name), &child,
                            &type);
        if (rc < 0)
            return rc;
        if (rc == 0)
            break;

        rc = inode_read_nolock(child, &child_inode);
        if (rc != 0)
            return rc;
        child_type = child_inode.mode & JEXFS3_TYPE_MASK;
        if (type == JEXFS3_DIRENT_DIR && child_type != JEXFS3_TYPE_DIR)
            return -EINVAL;
        if (type == JEXFS3_DIRENT_FILE && child_type != JEXFS3_TYPE_FILE)
            return -EINVAL;

        if (strcmp(name, ".") == 0) {
            if (child != index)
                return -EINVAL;
            has_dot = 1;
        } else if (strcmp(name, "..") == 0) {
            if (child_type != JEXFS3_TYPE_DIR)
                return -EINVAL;
            has_dotdot = 1;
        } else if (type == JEXFS3_DIRENT_DIR) {
            subdirs++;
        }

        if (inode_used_nolock(child) != 1)
            return -EINVAL;
        reachable[child / 8u] |= (uint8_t)(1u << (child % 8u));
    }

    if (!has_dot || !has_dotdot)
        return -EINVAL;
    if (inode->links != (uint16_t)(2 + subdirs))
        return -EINVAL;
    return 0;
}

static int verify_nolock(void)
{
    uint8_t *reachable;
    uint32_t bitmap_bytes;
    uint32_t index;
    int rc = 0;

    bitmap_bytes = (fs3_sb.inode_count + 7u) / 8u;
    reachable = (uint8_t *)kmalloc(bitmap_bytes);
    if (!reachable)
        return -ENOMEM;
    memset(reachable, 0, bitmap_bytes);

    if (inode_used_nolock(fs3_sb.root_inode) != 1) {
        rc = -EINVAL;
        goto out;
    }
    reachable[fs3_sb.root_inode / 8u] |=
        (uint8_t)(1u << (fs3_sb.root_inode % 8u));

    for (index = 0; index < fs3_sb.inode_count; index++) {
        struct jexfs3_inode inode;
        uint16_t type;
        uint32_t data_blocks;
        uint32_t crc = 0;
        uint64_t pos = 0;

        rc = inode_used_nolock(index);
        if (rc < 0)
            goto out;
        if (rc == 0)
            continue;

        rc = inode_read_nolock(index, &inode);
        if (rc != 0)
            goto out;
        type = inode.mode & JEXFS3_TYPE_MASK;
        if (type != JEXFS3_TYPE_FILE && type != JEXFS3_TYPE_DIR) {
            rc = -EINVAL;
            goto out;
        }
        if (inode.links == 0) {
            rc = -EINVAL;
            goto out;
        }

        rc = extents_check_nolock(&inode, &data_blocks);
        if (rc != 0)
            goto out;
        (void)data_blocks;

        while (pos < inode.size) {
            uint8_t chunk_buf[256];
            uint32_t chunk = sizeof(chunk_buf);
            int got;

            if (inode.size - pos < chunk)
                chunk = (uint32_t)(inode.size - pos);
            got = read_nolock(&inode, chunk_buf, chunk, pos);
            if (got != (int)chunk) {
                rc = -EIO;
                goto out;
            }
            crc = jexfs3_crc32c(crc, chunk_buf, chunk);
            pos += chunk;
        }
        if (crc != inode.data_crc) {
            rc = -EINVAL;
            goto out;
        }

        if (type == JEXFS3_TYPE_DIR) {
            rc = verify_dir_nolock(index, &inode, reachable);
            if (rc != 0)
                goto out;
        } else if (inode.links != 1) {
            rc = -EINVAL;
            goto out;
        }
    }

    for (index = 0; index < fs3_sb.inode_count; index++) {
        int used = inode_used_nolock(index);

        if (used < 0) {
            rc = used;
            goto out;
        }
        if (used && !(reachable[index / 8u] & (1u << (index % 8u)))) {
            rc = -EINVAL;
            goto out;
        }
    }

out:
    kfree(reachable);
    return rc;
}

int jexfs3_verify(void)
{
    int rc;

    if (!fs3_mounted)
        return -ENODEV;
    mutex_lock(&fs3_lock);
    rc = verify_nolock();
    mutex_unlock(&fs3_lock);
    return rc;
}
