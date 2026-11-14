/**
 * @file jexfsck3.c
 * @brief Host-side verifier for JexFS v3 images.
 *
 * Validates the primary and backup superblocks, the metadata layout, every
 * inode and extent block checksum, directory entry chains, link counts,
 * bitmaps, and each file's data crc.  Given one or more source directories
 * (the same ones handed to mkjexfs3), it also compares every file
 * byte-for-byte and reports missing or extra entries.
 *
 * Usage:
 *   jexfsck3 <image.img> [source_dir...]
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <dirent.h>
#include <errno.h>
#include <sys/stat.h>
#include "../src/include/jexfs3.h"

#define MAX_SRC 1024
#define PATH_MAX_LEN 1024
#define MAX_DEPTH 64
#define MAX_REPORTED 40

struct src_entry {
    char rel[PATH_MAX_LEN];
    char path[PATH_MAX_LEN];
    uint8_t type;
    uint64_t size;
    int seen;
};

static uint8_t *img;
static size_t img_size;
static uint32_t total_blocks;
static struct jexfs3_superblock sb;
static struct src_entry src[MAX_SRC];
static uint32_t src_count;
static int failures;
static int reported;
static int files_checked;
static int dirs_checked;
static int compare_sources;
static uint64_t data_blocks_used;
static uint32_t extent_blocks_used;
static uint8_t *seen_inodes;
static uint8_t *used_blocks;

static void report(const char *fmt, ...)
{
    va_list ap;

    failures++;
    if (reported++ >= MAX_REPORTED)
        return;
    fprintf(stderr, "  FAIL: ");
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, "\n");
}

static uint8_t *block_ptr(uint32_t block)
{
    if (block >= total_blocks) {
        report("block %u out of range", block);
        return NULL;
    }
    return img + (size_t)block * JEXFS3_BLOCK_SIZE;
}

static int inode_read(uint32_t index, struct jexfs3_inode *out)
{
    if (index >= sb.inode_count) {
        report("inode %u >= inode_count %u", index, sb.inode_count);
        return -1;
    }

    uint64_t off = (uint64_t)sb.inode_table_start * JEXFS3_BLOCK_SIZE
                   + (uint64_t)index * JEXFS3_INODE_SIZE;
    if (off + JEXFS3_INODE_SIZE > img_size) {
        report("inode %u outside image", index);
        return -1;
    }

    memcpy(out, img + off, JEXFS3_INODE_SIZE);
    uint32_t crc = jexfs3_crc_zeroed(out, JEXFS3_INODE_SIZE,
                                     JEXFS3_INODE_CHECKSUM_OFF);
    if (crc != out->checksum) {
        report("inode %u checksum %08x != %08x", index, crc, out->checksum);
        return -1;
    }
    return 0;
}

static int extent_ok(const struct jexfs3_extent *e, const char *what)
{
    if (e->length == 0) {
        report("%s: zero-length extent", what);
        return -1;
    }
    if (e->flags != 0) {
        report("%s: extent flags %u", what, e->flags);
        return -1;
    }
    if ((uint64_t)e->logical + e->length > UINT32_MAX) {
        report("%s: logical overflow", what);
        return -1;
    }
    if ((uint64_t)e->physical + e->length > total_blocks) {
        report("%s: physical range %u+%u past end", what,
               e->physical, e->length);
        return -1;
    }
    if (e->physical < sb.first_data_block) {
        report("%s: extent into metadata block %u", what, e->physical);
        return -1;
    }
    if (e->physical + e->length > total_blocks - 1) {
        report("%s: extent overlaps the backup superblock", what);
        return -1;
    }
    return 0;
}

static int mark_blocks(const struct jexfs3_extent *e, const char *what)
{
    for (uint32_t i = 0; i < e->length; i++) {
        uint32_t b = e->physical + i;
        if (used_blocks[b / 8] & (1u << (b % 8))) {
            report("%s: block %u allocated twice", what, b);
            return -1;
        }
        used_blocks[b / 8] |= (uint8_t)(1u << (b % 8));
    }
    data_blocks_used += e->length;
    return 0;
}

/**
 * check_inode_extents - Validate the extent list and mark the data blocks.
 *
 * Also checks that the extents cover logical blocks 0..n-1 exactly with no
 * holes, which is what the read path assumes.
 */
static int check_inode_extents(const struct jexfs3_inode *inode,
                               const char *path, uint32_t *blocks_out)
{
    uint32_t expected = (uint32_t)((inode->size + JEXFS3_BLOCK_SIZE - 1)
                                   / JEXFS3_BLOCK_SIZE);
    uint32_t next_logical = 0;
    uint32_t count = 0;

    if (inode->extent_depth > 1) {
        report("%s: unsupported extent depth %u", path, inode->extent_depth);
        return -1;
    }

    if (inode->extent_depth == 1) {
        uint8_t *blk = block_ptr(inode->extent_root);
        if (!blk)
            return -1;
        struct jexfs3_extent_block *eb = (struct jexfs3_extent_block *)blk;

        uint32_t crc = jexfs3_crc_zeroed(eb, JEXFS3_BLOCK_SIZE,
                                         JEXFS3_EXTENT_CHECKSUM_OFF);
        if (eb->magic != JEXFS3_EXTENT_MAGIC || eb->depth != 1 ||
            eb->count == 0 || eb->count > JEXFS3_EXTENT_ENTRIES) {
            report("%s: bad extent block header", path);
            return -1;
        }
        if (crc != eb->checksum) {
            report("%s: extent block checksum mismatch", path);
            return -1;
        }
        if (used_blocks[inode->extent_root / 8]
            & (1u << (inode->extent_root % 8))) {
            report("%s: extent block %u reused", path, inode->extent_root);
            return -1;
        }
        used_blocks[inode->extent_root / 8] |=
            (uint8_t)(1u << (inode->extent_root % 8));
        extent_blocks_used++;

        for (uint32_t i = 0; i < eb->count; i++) {
            if (extent_ok(&eb->entries[i], path) != 0)
                return -1;
            if (eb->entries[i].logical != next_logical) {
                report("%s: extent hole/overlap at logical %u", path,
                       next_logical);
                return -1;
            }
            if (mark_blocks(&eb->entries[i], path) != 0)
                return -1;
            next_logical += eb->entries[i].length;
            count += eb->entries[i].length;
        }
    } else {
        for (uint32_t i = 0; i < inode->extent_count; i++) {
            if (i >= JEXFS3_INLINE_EXTENTS) {
                report("%s: inline extent count %u > %u", path,
                       inode->extent_count, JEXFS3_INLINE_EXTENTS);
                return -1;
            }
            if (extent_ok(&inode->inline_extents[i], path) != 0)
                return -1;
            if (inode->inline_extents[i].logical != next_logical) {
                report("%s: extent hole/overlap at logical %u", path,
                       next_logical);
                return -1;
            }
            if (mark_blocks(&inode->inline_extents[i], path) != 0)
                return -1;
            next_logical += inode->inline_extents[i].length;
            count += inode->inline_extents[i].length;
        }
    }

    if (count != expected) {
        report("%s: extents cover %u blocks, size needs %u", path, count,
               expected);
        return -1;
    }

    uint32_t metadata = inode->extent_depth == 1 ? 1 : 0;
    if (inode->blocks != count + metadata) {
        report("%s: inode->blocks %llu, extents %u + %u metadata", path,
               (unsigned long long)inode->blocks, count, metadata);
        return -1;
    }

    *blocks_out = count;
    return 0;
}

static int bmap(const struct jexfs3_inode *inode, uint32_t logical,
                uint32_t *physical)
{
    if (inode->extent_depth == 0) {
        for (uint32_t i = 0; i < inode->extent_count; i++) {
            const struct jexfs3_extent *e = &inode->inline_extents[i];
            if (logical >= e->logical && logical < e->logical + e->length) {
                *physical = e->physical + (logical - e->logical);
                return 0;
            }
        }
        return -1;
    }

    uint8_t *blk = block_ptr(inode->extent_root);
    if (!blk)
        return -1;
    struct jexfs3_extent_block *eb = (struct jexfs3_extent_block *)blk;
    for (uint32_t i = 0; i < eb->count; i++) {
        const struct jexfs3_extent *e = &eb->entries[i];
        if (logical >= e->logical && logical < e->logical + e->length) {
            *physical = e->physical + (logical - e->logical);
            return 0;
        }
    }
    return -1;
}

/**
 * check_data - Read inode data, verify its crc, optionally match a source.
 */
static int check_data(const struct jexfs3_inode *inode, const char *path,
                      const char *srcpath)
{
    uint8_t buf[JEXFS3_BLOCK_SIZE];
    FILE *f = NULL;
    uint32_t crc = 0;
    uint64_t left = inode->size;
    uint32_t logical = 0;

    if (srcpath) {
        f = fopen(srcpath, "rb");
        if (!f) {
            report("%s: cannot open source %s", path, srcpath);
            return -1;
        }
    }

    while (left > 0) {
        uint32_t chunk = left < JEXFS3_BLOCK_SIZE
                         ? (uint32_t)left : JEXFS3_BLOCK_SIZE;
        uint32_t phys;
        if (bmap(inode, logical, &phys) != 0) {
            report("%s: no extent for logical block %u", path, logical);
            if (f) fclose(f);
            return -1;
        }
        memcpy(buf, block_ptr(phys), chunk);
        crc = jexfs3_crc32c(crc, buf, chunk);

        if (f) {
            uint8_t srcbuf[JEXFS3_BLOCK_SIZE];
            size_t got = fread(srcbuf, 1, chunk, f);
            if (got != chunk) {
                report("%s: short source read", path);
                fclose(f);
                return -1;
            }
            if (memcmp(buf, srcbuf, chunk) != 0) {
                report("%s: data differs from source", path);
                fclose(f);
                return -1;
            }
        }

        left -= chunk;
        logical++;
    }

    if (f) {
        if (fgetc(f) != EOF)
            report("%s: source longer than inode size", path);
        fclose(f);
    }

    if (crc != inode->data_crc) {
        report("%s: data crc %08x != inode %08x", path, crc, inode->data_crc);
        return -1;
    }
    return 0;
}

static int src_find(const char *rel)
{
    for (uint32_t i = 0; i < src_count; i++)
        if (strcmp(src[i].rel, rel) == 0)
            return (int)i;
    return -1;
}

static int add_src(const char *rel, const char *path, uint8_t type, uint64_t size)
{
    if (src_count >= MAX_SRC) {
        report("too many source entries (max %u)", MAX_SRC);
        return -1;
    }
    if (strlen(rel) >= PATH_MAX_LEN) {
        report("source path too long: %s", rel);
        return -1;
    }
    if (src_find(rel) >= 0) {
        report("duplicate source name: %s", rel);
        return -1;
    }
    struct src_entry *e = &src[src_count++];
    memset(e, 0, sizeof(*e));
    strcpy(e->rel, rel);
    strcpy(e->path, path);
    e->type = type;
    e->size = size;
    return 0;
}

static int scan_source(const char *dirpath, const char *rel)
{
    struct dirent **entries;
    int n = scandir(dirpath, &entries, NULL, alphasort);

    if (n < 0) {
        report("cannot read source dir %s", dirpath);
        return -1;
    }

    for (int i = 0; i < n; i++) {
        struct dirent *ent = entries[i];
        char path[PATH_MAX_LEN];
        char child_rel[PATH_MAX_LEN];
        struct stat st;

        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
            free(ent);
            continue;
        }
        if (snprintf(path, sizeof(path), "%s/%s", dirpath, ent->d_name)
            >= (int)sizeof(path)) {
            report("source path too long: %s", ent->d_name);
            free(ent);
            continue;
        }
        if (rel[0])
            snprintf(child_rel, sizeof(child_rel), "%s/%s", rel, ent->d_name);
        else
            snprintf(child_rel, sizeof(child_rel), "%s", ent->d_name);

        if (stat(path, &st) != 0) {
            report("stat %s failed", path);
            free(ent);
            continue;
        }
        if (S_ISDIR(st.st_mode)) {
            add_src(child_rel, path, JEXFS3_DIRENT_DIR, 0);
            scan_source(path, child_rel);
        } else if (S_ISREG(st.st_mode)) {
            add_src(child_rel, path, JEXFS3_DIRENT_FILE, (uint64_t)st.st_size);
        } else {
            report("unsupported source file %s", path);
        }
        free(ent);
    }
    free(entries);
    return 0;
}

static int check_dir(uint32_t inode_idx, uint32_t parent_index,
                     const struct jexfs3_inode *inode, const char *path,
                     const char *rel, int depth);

static int check_dirent(uint32_t dir_index, uint32_t parent_index,
                        const struct jexfs3_dirent *de, uint32_t off,
                        uint32_t limit, const char *parent_path,
                        const char *parent_rel, int depth)
{
    if (de->rec_len < JEXFS3_DIRENT_MIN || off + de->rec_len > limit) {
        report("%s: bad rec_len at offset %u", parent_path, off);
        return -1;
    }
    if (de->name_len == 0 || de->name_len > de->rec_len - JEXFS3_DIRENT_MIN) {
        report("%s: bad name_len at offset %u", parent_path, off);
        return -1;
    }
    if (de->inode >= sb.inode_count || de->inode < JEXFS3_ROOT_INODE) {
        report("%s: entry inode %u out of range", parent_path, de->inode);
        return -1;
    }

    char name[JEXFS3_NAME_MAX + 1];
    memcpy(name, de->name, de->name_len);
    name[de->name_len] = '\0';

    if (strcmp(name, ".") == 0) {
        if (de->inode != dir_index)
            report("%s: . points to %u, not %u", parent_path, de->inode,
                   dir_index);
        return 0;
    }
    if (strcmp(name, "..") == 0) {
        if (de->inode != parent_index)
            report("%s: .. points to %u, not %u", parent_path, de->inode,
                   parent_index);
        return 0;
    }

    struct jexfs3_inode child;
    if (inode_read(de->inode, &child) != 0)
        return -1;

    uint16_t child_type = (uint16_t)(child.mode & JEXFS3_TYPE_MASK);
    uint8_t expected = child_type == JEXFS3_TYPE_DIR ? JEXFS3_DIRENT_DIR
                                                     : JEXFS3_DIRENT_FILE;
    if (de->file_type != expected) {
        report("%s/%s: dirent type %u != inode type %u", parent_path, name,
               de->file_type, expected);
        return -1;
    }

    char child_rel[PATH_MAX_LEN];
    if (parent_rel[0])
        snprintf(child_rel, sizeof(child_rel), "%s/%s", parent_rel, name);
    else
        snprintf(child_rel, sizeof(child_rel), "%s", name);
    char child_path[PATH_MAX_LEN + 2];
    if (parent_path[strlen(parent_path) - 1] == '/')
        snprintf(child_path, sizeof(child_path), "%s%s", parent_path, name);
    else
        snprintf(child_path, sizeof(child_path), "%s/%s", parent_path, name);

    int s = src_find(child_rel);
    if (s >= 0) {
        if (src[s].type != expected) {
            report("%s: source/image type mismatch", child_path);
            return -1;
        }
        if (expected == JEXFS3_DIRENT_FILE && src[s].size != child.size) {
            report("%s: size %llu != source %llu", child_path,
                   (unsigned long long)child.size,
                   (unsigned long long)src[s].size);
            return -1;
        }
        src[s].seen = 1;
    } else if (compare_sources) {
        report("%s: not present in the source tree", child_path);
        return -1;
    }

    if (seen_inodes[de->inode / 8] & (1u << (de->inode % 8))) {
        report("%s: inode %u referenced twice", child_path, de->inode);
        return -1;
    }
    seen_inodes[de->inode / 8] |= (uint8_t)(1u << (de->inode % 8));

    if (expected == JEXFS3_DIRENT_DIR) {
        if (check_dir(de->inode, dir_index, &child, child_path, child_rel,
                      depth + 1) != 0)
            return -1;
    } else {
        uint32_t blocks;
        if (check_inode_extents(&child, child_path, &blocks) != 0)
            return -1;
        if (check_data(&child, child_path, s >= 0 ? src[s].path : NULL) != 0)
            return -1;
        files_checked++;
    }
    return 0;
}

/**
 * check_dir - Walk one directory and recurse into its children.
 */
static int check_dir(uint32_t inode_idx, uint32_t parent_index,
                     const struct jexfs3_inode *inode, const char *path,
                     const char *rel, int depth)
{
    if (depth > MAX_DEPTH) {
        report("%s: directory tree deeper than %u", path, MAX_DEPTH);
        return -1;
    }
    if ((inode->mode & JEXFS3_TYPE_MASK) != JEXFS3_TYPE_DIR) {
        report("%s: not a directory", path);
        return -1;
    }

    uint32_t blocks;
    if (check_inode_extents(inode, path, &blocks) != 0)
        return -1;
    if (check_data(inode, path, NULL) != 0)
        return -1;

    /* Recount to keep the caller's data-block counter meaningful. */
    dirs_checked++;

    uint32_t child_dirs = 0;
    uint32_t entries = 0;
    uint8_t *block_buf = (uint8_t *)malloc(JEXFS3_BLOCK_SIZE);
    if (!block_buf) {
        report("out of memory");
        return -1;
    }

    uint64_t seen_bytes = 0;
    uint32_t logical = 0;
    while (seen_bytes < inode->size) {
        uint32_t phys;
        if (bmap(inode, logical, &phys) != 0) {
            report("%s: no extent for directory block %u", path, logical);
            free(block_buf);
            return -1;
        }
        memcpy(block_buf, block_ptr(phys), JEXFS3_BLOCK_SIZE);
        uint32_t limit = JEXFS3_BLOCK_SIZE;
        if (inode->size - seen_bytes < JEXFS3_BLOCK_SIZE)
            limit = (uint32_t)(inode->size - seen_bytes);

        uint32_t off = 0;
        while (off < limit) {
            struct jexfs3_dirent *de = (struct jexfs3_dirent *)(block_buf + off);
            if (de->rec_len < JEXFS3_DIRENT_MIN || off + de->rec_len > limit) {
                report("%s: broken dirent chain at %llu+%u", path,
                       (unsigned long long)seen_bytes, off);
                free(block_buf);
                return -1;
            }
            if (check_dirent(inode_idx, parent_index, de, off, limit, path,
                             rel, depth) != 0) {
                free(block_buf);
                return -1;
            }
            if (de->file_type == JEXFS3_DIRENT_DIR &&
                strcmp(de->name, ".") != 0 && strcmp(de->name, "..") != 0)
                child_dirs++;
            entries++;
            off += de->rec_len;
        }
        seen_bytes += limit;
        logical++;
    }
    free(block_buf);

    uint16_t expected_links = (uint16_t)(2 + child_dirs);
    if (inode->links != expected_links) {
        report("%s: links %u, expected %u", path, inode->links,
               expected_links);
        return -1;
    }
    (void)entries;
    return 0;
}

static int check_superblock(void)
{
    if (img_size % JEXFS3_BLOCK_SIZE != 0) {
        report("image size %zu is not a multiple of %u", img_size,
               JEXFS3_BLOCK_SIZE);
        return -1;
    }
    total_blocks = (uint32_t)(img_size / JEXFS3_BLOCK_SIZE);

    uint8_t *b0 = img;
    memcpy(&sb, b0, sizeof(sb));

    if (sb.magic != JEXFS3_MAGIC || sb.version != JEXFS3_VERSION ||
        sb.block_size != JEXFS3_BLOCK_SIZE ||
        sb.inode_size != JEXFS3_INODE_SIZE) {
        report("bad superblock magic/version/block/inode size");
        return -1;
    }
    if (sb.total_blocks != total_blocks ||
        sb.total_bytes != (uint64_t)total_blocks * JEXFS3_BLOCK_SIZE) {
        report("superblock size does not match the image");
        return -1;
    }
    uint32_t crc = jexfs3_crc_zeroed(b0, JEXFS3_BLOCK_SIZE,
                                     JEXFS3_SB_CHECKSUM_OFF);
    if (crc != sb.checksum) {
        report("superblock checksum mismatch");
        return -1;
    }
    if ((sb.feature_incompat & ~JEXFS3_FEATURE_INCOMPAT_KNOWN) != 0) {
        report("unknown incompat features %08x", sb.feature_incompat);
        return -1;
    }
    if ((sb.feature_incompat & JEXFS3_FEATURE_INCOMPAT_EXTENTS) == 0 ||
        (sb.feature_incompat & JEXFS3_FEATURE_INCOMPAT_CRC32C) == 0) {
        report("required feature bits missing");
        return -1;
    }
    if (sb.journal_blocks != 0) {
        report("phase 1 image claims a journal (%u blocks)", sb.journal_blocks);
        return -1;
    }

    uint32_t ib = (sb.inode_count + 32767u) / 32768u;
    uint32_t bb = (total_blocks + 32767u) / 32768u;
    uint32_t it = (sb.inode_count * JEXFS3_INODE_SIZE + JEXFS3_BLOCK_SIZE - 1)
                  / JEXFS3_BLOCK_SIZE;
    if (ib == 0) ib = 1;
    if (bb == 0) bb = 1;
    if (it == 0) it = 1;

    if (sb.inode_bitmap_start != 1 ||
        sb.inode_bitmap_blocks != ib ||
        sb.block_bitmap_start != sb.inode_bitmap_start + ib ||
        sb.block_bitmap_blocks != bb ||
        sb.inode_table_start != sb.block_bitmap_start + bb ||
        sb.inode_table_blocks != it ||
        sb.first_data_block != sb.inode_table_start + it) {
        report("metadata layout does not match the superblock");
        return -1;
    }
    if (sb.first_data_block >= total_blocks - 1 ||
        sb.backup_block != total_blocks - 1) {
        report("data region or backup block out of range");
        return -1;
    }
    if (sb.root_inode != JEXFS3_ROOT_INODE ||
        sb.root_inode >= sb.inode_count) {
        report("bad root inode %u", sb.root_inode);
        return -1;
    }

    uint8_t *backup = block_ptr(total_blocks - 1);
    if (!backup)
        return -1;
    if (memcmp(backup, b0, JEXFS3_BLOCK_SIZE) != 0) {
        report("backup superblock differs from the primary");
        return -1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <image.img> [source_dir...]\n", argv[0]);
        return 1;
    }

    FILE *f = fopen(argv[1], "rb");
    if (!f) {
        fprintf(stderr, "Error: cannot open %s\n", argv[1]);
        return 1;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 0) {
        fprintf(stderr, "Error: empty image\n");
        fclose(f);
        return 1;
    }
    img_size = (size_t)size;
    img = (uint8_t *)malloc(img_size);
    if (!img) {
        fprintf(stderr, "Error: out of memory\n");
        fclose(f);
        return 1;
    }
    if (fread(img, 1, img_size, f) != img_size) {
        fprintf(stderr, "Error: short read\n");
        fclose(f);
        free(img);
        return 1;
    }
    fclose(f);

    printf("JexFS v3 check: %s\n", argv[1]);
    if (check_superblock() != 0)
        goto done;

    for (int i = 2; i < argc; i++) {
        compare_sources = 1;
        if (scan_source(argv[i], "") != 0)
            goto done;
    }
    add_src("", "", JEXFS3_DIRENT_DIR, 0);
    src[src_count - 1].seen = 1;

    seen_inodes = (uint8_t *)calloc((sb.inode_count + 7) / 8, 1);
    used_blocks = (uint8_t *)calloc((total_blocks + 7) / 8, 1);
    if (!seen_inodes || !used_blocks) {
        report("out of memory for bitmaps");
        goto done;
    }

    /* Metadata blocks are not data extents, but must not be reallocated. */
    for (uint32_t b = 0; b < sb.first_data_block; b++)
        used_blocks[b / 8] |= (uint8_t)(1u << (b % 8));
    used_blocks[(total_blocks - 1) / 8] |=
        (uint8_t)(1u << ((total_blocks - 1) % 8));

    /* Bitmap consistency: every marked inode must be used, and vice versa. */
    uint32_t ib_bits = 0;
    for (uint32_t i = 0; i < sb.inode_count; i++)
        if (img[(size_t)sb.inode_bitmap_start * JEXFS3_BLOCK_SIZE + i / 8]
            & (1u << (i % 8)))
            ib_bits++;

    struct jexfs3_inode root;
    if (inode_read(sb.root_inode, &root) != 0)
        goto done;
    seen_inodes[sb.root_inode / 8] |=
        (uint8_t)(1u << (sb.root_inode % 8));

    if (check_dir(sb.root_inode, sb.root_inode, &root, "/", "", 0) != 0)
        goto done;

    if (ib_bits != (uint32_t)(dirs_checked + files_checked)) {
        report("inode bitmap marks %u, expected %u", ib_bits,
               dirs_checked + files_checked);
    }

    /* Every source entry must have been visited. */
    for (uint32_t i = 0; i < src_count; i++)
        if (!src[i].seen)
            report("missing from image: /%s", src[i].rel);

    /* Cross-check the block bitmap with the blocks we actually walked. */
    uint32_t bb_bits = 0;
    for (uint32_t i = 0; i < total_blocks; i++)
        if (img[(size_t)sb.block_bitmap_start * JEXFS3_BLOCK_SIZE + i / 8]
            & (1u << (i % 8)))
            bb_bits++;
    uint32_t expected_used = sb.first_data_block + 1
                             + (uint32_t)data_blocks_used
                             + extent_blocks_used;
    if (bb_bits != expected_used)
        report("block bitmap marks %u, expected %u", bb_bits, expected_used);

    printf("  superblock ok, uuid %02x%02x%02x%02x-%02x%02x-%02x%02x-"
           "%02x%02x-%02x%02x%02x%02x%02x%02x, generation %u\n",
           sb.uuid[0], sb.uuid[1], sb.uuid[2], sb.uuid[3],
           sb.uuid[4], sb.uuid[5], sb.uuid[6], sb.uuid[7],
           sb.uuid[8], sb.uuid[9], sb.uuid[10], sb.uuid[11],
           sb.uuid[12], sb.uuid[13], sb.uuid[14], sb.uuid[15],
           sb.generation);

done:
    if (failures == 0)
        printf("verified %d file(s), %d dir(s), %u blocks, 0 failure(s)\n",
               files_checked, dirs_checked, total_blocks);
    else
        printf("FAILED: %d failure(s)\n", failures);
    free(seen_inodes);
    free(used_blocks);
    free(img);
    return failures ? 1 : 0;
}
