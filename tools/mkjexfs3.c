/**
 * @file mkjexfs3.c
 * @brief Host tool to create a JexFS v3 disk image.
 *
 * Packs regular files and directories into the 4 KB-block, u32-block-number
 * format defined by src/include/jexfs3.h.  Files are stored as extents: up
 * to four inline in the inode, otherwise one leaf extent block.  Every
 * superblock, inode and extent block carries a crc32c; file and directory
 * data are covered by the inode's data_crc.
 *
 * Usage:
 *   mkjexfs3 <output.img> <block_count> [--split=N] <source_dir...>
 *
 * --split=N caps each extent at N blocks.  It exists to exercise the leaf
 * extent path in tests; production images leave it unset so each file is
 * one contiguous extent.  Multiple source directories are merged into the
 * image root, which is how build-generated assets get in.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <dirent.h>
#include <errno.h>
#include <sys/stat.h>
#include "../src/include/jexfs3.h"

#define MAX_NODES 1024
#define PATH_MAX_LEN 1024
#define NAME_MAX_LEN 256

struct node {
    char     name[NAME_MAX_LEN];
    char     rel[PATH_MAX_LEN];   /* path from image root, for the UUID */
    char     src[PATH_MAX_LEN];
    uint32_t parent;
    uint32_t inode;
    uint32_t links;
    uint8_t  type;                /* JEXFS3_DIRENT_FILE / _DIR */
    uint64_t size;
    uint32_t data_start;
    uint32_t data_blocks;
    uint32_t data_crc;
    uint32_t ext_count;
    uint32_t ext_root;
    uint16_t ext_depth;
    struct jexfs3_extent inline_ext[JEXFS3_INLINE_EXTENTS];
};

static uint8_t *img;
static uint32_t total_blocks;
static uint32_t next_block;
static uint32_t next_inode;
static uint32_t max_extent_blocks;
static struct node nodes[MAX_NODES];
static uint32_t node_count;

static void die(const char *msg)
{
    fprintf(stderr, "Error: %s\n", msg);
    exit(1);
}

static uint32_t align4(uint32_t v)
{
    return (v + 3u) & ~3u;
}

static struct jexfs3_superblock *sb_ptr(void)
{
    return (struct jexfs3_superblock *)img;
}

static uint8_t *block_ptr(uint32_t block)
{
    return img + (size_t)block * JEXFS3_BLOCK_SIZE;
}

static struct jexfs3_inode *inode_ptr(uint32_t inode)
{
    struct jexfs3_superblock *sb = sb_ptr();
    return (struct jexfs3_inode *)(
        img + (size_t)sb->inode_table_start * JEXFS3_BLOCK_SIZE
            + (size_t)inode * JEXFS3_INODE_SIZE);
}

static void bitmap_set(uint32_t block, uint32_t index)
{
    img[(size_t)block * JEXFS3_BLOCK_SIZE + index / 8] |= (uint8_t)(1u << (index % 8));
}

static void inode_bitmap_set(uint32_t index)
{
    bitmap_set(sb_ptr()->inode_bitmap_start, index);
}

static void block_bitmap_set(uint32_t index)
{
    bitmap_set(sb_ptr()->block_bitmap_start, index);
}

/* ------------------------------------------------------------------ */
/*  Path tree                                                          */
/* ------------------------------------------------------------------ */

static uint32_t add_node(const char *name, const char *rel, const char *src,
                         uint32_t parent, uint8_t type, uint64_t size)
{
    if (node_count >= MAX_NODES)
        die("too many files/directories (max 1024)");
    if (strlen(name) >= NAME_MAX_LEN)
        die("file name too long");
    if (strlen(rel) >= PATH_MAX_LEN)
        die("path too long");

    struct node *n = &nodes[node_count];
    memset(n, 0, sizeof(*n));
    strcpy(n->name, name);
    strcpy(n->rel, rel);
    strcpy(n->src, src);
    n->parent = parent;
    n->inode = next_inode++;
    n->type = type;
    n->size = size;
    return node_count++;
}

static int find_child(uint32_t parent, const char *name)
{
    for (uint32_t i = 0; i < node_count; i++)
        if (nodes[i].parent == parent && strcmp(nodes[i].name, name) == 0)
            return (int)i;
    return -1;
}

static int scan_dir(const char *srcpath, const char *rel, uint32_t parent)
{
    struct dirent **entries;
    int n = scandir(srcpath, &entries, NULL, alphasort);

    if (n < 0) {
        fprintf(stderr, "Warning: cannot read %s (%s)\n", srcpath, strerror(errno));
        return -1;
    }

    for (int i = 0; i < n; i++) {
        struct dirent *ent = entries[i];
        char src[PATH_MAX_LEN];
        char child_rel[PATH_MAX_LEN];

        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
            free(ent);
            continue;
        }
        if (snprintf(src, sizeof(src), "%s/%s", srcpath, ent->d_name) >= (int)sizeof(src)) {
            fprintf(stderr, "Warning: path too long for %s\n", ent->d_name);
            free(ent);
            continue;
        }
        if (rel[0])
            snprintf(child_rel, sizeof(child_rel), "%s/%s", rel, ent->d_name);
        else
            snprintf(child_rel, sizeof(child_rel), "%s", ent->d_name);

        if (find_child(parent, ent->d_name) >= 0)
            die("duplicate name in one directory (merged source roots?)");

        struct stat st;
        if (stat(src, &st) != 0) {
            fprintf(stderr, "Warning: stat %s failed (%s)\n", src, strerror(errno));
            free(ent);
            continue;
        }

        if (S_ISDIR(st.st_mode)) {
            uint32_t idx = add_node(ent->d_name, child_rel, src, parent,
                                    JEXFS3_DIRENT_DIR, 0);
            scan_dir(src, child_rel, idx);
        } else if (S_ISREG(st.st_mode)) {
            add_node(ent->d_name, child_rel, src, parent,
                     JEXFS3_DIRENT_FILE, (uint64_t)st.st_size);
        } else {
            fprintf(stderr, "Warning: skipping non-regular file %s\n", src);
        }
        free(ent);
    }
    free(entries);
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Data and inodes                                                    */
/* ------------------------------------------------------------------ */

static uint32_t alloc_blocks(uint32_t count)
{
    uint32_t start = next_block;
    if ((uint64_t)next_block + count >= total_blocks)
        die("image full while allocating data blocks");
    next_block += count;
    return start;
}

static uint32_t dir_entry_len(const char *name)
{
    return align4((uint32_t)sizeof(struct jexfs3_dirent) + (uint32_t)strlen(name));
}

static uint32_t count_children(uint32_t parent)
{
    uint32_t count = 0;
    for (uint32_t i = 0; i < node_count; i++)
        if (nodes[i].parent == parent && i != parent)
            count++;
    return count;
}

/**
 * fill_dir_data - Write one directory's entries into freshly allocated blocks.
 *
 * The entry list starts with "." and ".."; the final entry in each block is
 * stretched to the block boundary so the rec_len chain covers the whole
 * directory size, including padding.
 */
static void fill_dir_data(struct node *dir)
{
    uint32_t entry_count = 2 + count_children((uint32_t)(dir - nodes));
    uint32_t bytes = 0;

    bytes += dir_entry_len(".");
    bytes += dir_entry_len("..");
    for (uint32_t i = 0; i < node_count; i++)
        if (nodes[i].parent == (uint32_t)(dir - nodes) &&
            i != (uint32_t)(dir - nodes))
            bytes += dir_entry_len(nodes[i].name);

    uint32_t blocks = (bytes + JEXFS3_BLOCK_SIZE - 1) / JEXFS3_BLOCK_SIZE;
    if (blocks == 0)
        blocks = 1;

    dir->data_start  = alloc_blocks(blocks);
    dir->data_blocks = blocks;
    dir->size        = (uint64_t)blocks * JEXFS3_BLOCK_SIZE;

    /* Directories are contiguous by construction, so one inline extent
     * describes the whole thing. */
    dir->ext_count = 1;
    dir->inline_ext[0].logical  = 0;
    dir->inline_ext[0].physical = dir->data_start;
    dir->inline_ext[0].length   = (uint16_t)blocks;

    uint32_t written = 0;   /* entries written across all blocks */
    for (uint32_t b = 0; b < blocks; b++) {
        uint8_t *blk = block_ptr(dir->data_start + b);
        uint32_t off = 0;
        uint32_t last_off = 0;
        int have_last = 0;

        while (written < entry_count) {
            const char *name;
            uint32_t child = 0;
            uint8_t file_type;

            if (written == 0) {
                name = ".";
                child = dir->inode;
                file_type = JEXFS3_DIRENT_DIR;
            } else if (written == 1) {
                name = "..";
                child = nodes[dir->parent].inode;
                file_type = JEXFS3_DIRENT_DIR;
            } else {
                /* Children in node order, which scan_dir sorted per level. */
                uint32_t seen = 2;
                uint32_t i;
                for (i = 0; i < node_count; i++) {
                    if (nodes[i].parent != (uint32_t)(dir - nodes) ||
                        i == (uint32_t)(dir - nodes))
                        continue;
                    if (seen++ == written)
                        break;
                }
                name = nodes[i].name;
                child = nodes[i].inode;
                file_type = nodes[i].type;
            }

            uint32_t len = dir_entry_len(name);
            if (off + len > JEXFS3_BLOCK_SIZE)
                break;   /* stretch the previous entry and start a new block */

            struct jexfs3_dirent *de = (struct jexfs3_dirent *)(blk + off);
            de->inode = child;
            de->rec_len = (uint16_t)len;
            de->name_len = (uint8_t)strlen(name);
            de->file_type = file_type;
            memcpy(de->name, name, de->name_len);

            last_off = off;
            have_last = 1;
            off += len;
            written++;
        }

        if (have_last) {
            struct jexfs3_dirent *last = (struct jexfs3_dirent *)(blk + last_off);
            last->rec_len = (uint16_t)(JEXFS3_BLOCK_SIZE - last_off);
        }
    }

    if (written != entry_count)
        die("directory entries did not fit (internal error)");

    dir->data_crc = jexfs3_crc32c(0, block_ptr(dir->data_start),
                                  blocks * JEXFS3_BLOCK_SIZE);
}

static void fill_file_data(struct node *file)
{
    uint32_t blocks = (uint32_t)((file->size + JEXFS3_BLOCK_SIZE - 1)
                                 / JEXFS3_BLOCK_SIZE);
    struct jexfs3_extent *exts = NULL;

    file->data_blocks = blocks;
    if (blocks == 0)
        return;

    file->data_start = alloc_blocks(blocks);

    FILE *f = fopen(file->src, "rb");
    if (!f)
        die("cannot reopen source file");

    uint32_t crc = 0;
    uint64_t left = file->size;
    for (uint32_t b = 0; b < blocks; b++) {
        size_t want = left < JEXFS3_BLOCK_SIZE ? (size_t)left : JEXFS3_BLOCK_SIZE;
        size_t got = fread(block_ptr(file->data_start + b), 1, want, f);
        if (got != want) {
            fprintf(stderr, "Error: short read on %s\n", file->src);
            exit(1);
        }
        crc = jexfs3_crc32c(crc, block_ptr(file->data_start + b), (uint32_t)got);
        left -= got;
    }
    fclose(f);
    file->data_crc = crc;

    uint32_t max_run = max_extent_blocks ? max_extent_blocks : 65535u;
    uint32_t extent_count = (blocks + max_run - 1) / max_run;
    exts = (struct jexfs3_extent *)calloc(extent_count, sizeof(*exts));
    if (!exts)
        die("out of memory for extents");

    uint32_t logical = 0;
    uint32_t n = 0;
    while (logical < blocks) {
        uint32_t run = blocks - logical;
        if (run > max_run)
            run = max_run;
        exts[n].logical  = logical;
        exts[n].physical = file->data_start + logical;
        exts[n].length   = (uint16_t)run;
        n++;
        logical += run;
    }
    file->ext_count = n;

    if (n <= JEXFS3_INLINE_EXTENTS) {
        memcpy(file->inline_ext, exts, n * sizeof(*exts));
    } else if (n <= JEXFS3_EXTENT_ENTRIES) {
        uint32_t leaf = alloc_blocks(1);
        struct jexfs3_extent_block *eb =
            (struct jexfs3_extent_block *)block_ptr(leaf);
        memset(eb, 0, JEXFS3_BLOCK_SIZE);
        eb->magic = JEXFS3_EXTENT_MAGIC;
        eb->depth = 1;
        eb->count = (uint16_t)n;
        memcpy(eb->entries, exts, n * sizeof(*exts));
        eb->checksum = jexfs3_crc_zeroed(eb, JEXFS3_BLOCK_SIZE,
                                         JEXFS3_EXTENT_CHECKSUM_OFF);
        file->ext_root  = leaf;
        file->ext_depth = 1;
    } else {
        fprintf(stderr, "Error: %s needs %u extents; phase 1 supports %u\n",
                file->rel, n, JEXFS3_EXTENT_ENTRIES);
        exit(1);
    }

    free(exts);
}

static uint32_t fnv1a(uint32_t hash, const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    while (len--) {
        hash ^= *p++;
        hash *= 16777619u;
    }
    return hash;
}

static void compute_uuid(uint8_t uuid[16])
{
    uint32_t h1 = 2166136261u;
    uint32_t h2 = 2166136261u;

    for (uint32_t i = 0; i < node_count; i++) {
        h1 = fnv1a(h1, nodes[i].rel, strlen(nodes[i].rel));
        h1 = fnv1a(h1, &nodes[i].type, 1);
        h1 = fnv1a(h1, &nodes[i].size, sizeof(nodes[i].size));
        h1 = fnv1a(h1, &nodes[i].data_crc, sizeof(nodes[i].data_crc));

        h2 = fnv1a(h2, nodes[i].name, strlen(nodes[i].name));
        h2 = fnv1a(h2, &nodes[i].inode, sizeof(nodes[i].inode));
    }

    memcpy(uuid, &h1, 4);
    memcpy(uuid + 4, &h2, 4);
    memcpy(uuid + 8, &h1, 4);
    memcpy(uuid + 12, &h2, 4);

    /* Mark it as a name-based UUID and set the RFC 4122 variant bits. */
    uuid[6] = (uint8_t)((uuid[6] & 0x0F) | 0x50);
    uuid[8] = (uint8_t)((uuid[8] & 0x3F) | 0x80);
}

/* ------------------------------------------------------------------ */
/*  Main                                                               */
/* ------------------------------------------------------------------ */

int main(int argc, char **argv)
{
    const char *dirs[16];
    uint32_t dir_count = 0;

    if (argc < 3) {
        fprintf(stderr,
                "Usage: %s <output.img> <block_count> [--split=N] <source_dir...>\n",
                argv[0]);
        return 1;
    }

    total_blocks = (uint32_t)strtoul(argv[2], NULL, 0);
    if (total_blocks < 32)
        die("block_count must be at least 32");

    for (int i = 3; i < argc; i++) {
        if (strncmp(argv[i], "--split=", 8) == 0)
            max_extent_blocks = (uint32_t)strtoul(argv[i] + 8, NULL, 0);
        else if (dir_count < 16)
            dirs[dir_count++] = argv[i];
        else
            die("too many source directories");
    }
    if (max_extent_blocks > 65535u)
        die("--split must be 1..65535");

    if ((uint64_t)total_blocks * JEXFS3_BLOCK_SIZE > (uint64_t)SIZE_MAX)
        die("image too large for this host");

    size_t image_size = (size_t)total_blocks * JEXFS3_BLOCK_SIZE;
    img = (uint8_t *)calloc(1, image_size);
    if (!img)
        die("out of memory for the image");

    /* Root is inode 2; 0 and 1 are reserved. */
    next_inode = JEXFS3_ROOT_INODE;
    node_count = 0;
    (void)add_node("", "", "", 0, JEXFS3_DIRENT_DIR, 0);

    if (dir_count == 0)
        dirs[dir_count++] = "rootfs";
    for (uint32_t i = 0; i < dir_count; i++)
        scan_dir(dirs[i], "", 0);

    printf("JexFS v3: %s  blocks=%u  size=%zu bytes\n",
           argv[1], total_blocks, image_size);

    /* Metadata layout. */
    uint32_t inode_count = next_inode;
    uint32_t ib_blocks = (inode_count + 32767u) / 32768u;
    uint32_t bb_blocks = (total_blocks + 32767u) / 32768u;
    uint32_t it_blocks = (inode_count * JEXFS3_INODE_SIZE + JEXFS3_BLOCK_SIZE - 1)
                         / JEXFS3_BLOCK_SIZE;

    if (ib_blocks == 0) ib_blocks = 1;
    if (bb_blocks == 0) bb_blocks = 1;
    if (it_blocks == 0) it_blocks = 1;

    struct jexfs3_superblock *sb = sb_ptr();
    sb->inode_bitmap_start = 1;
    sb->inode_bitmap_blocks = ib_blocks;
    sb->block_bitmap_start = sb->inode_bitmap_start + ib_blocks;
    sb->block_bitmap_blocks = bb_blocks;
    sb->inode_table_start = sb->block_bitmap_start + bb_blocks;
    sb->inode_table_blocks = it_blocks;
    sb->first_data_block = sb->inode_table_start + it_blocks;

    if (sb->first_data_block >= total_blocks - 1)
        die("block_count too small for the inode table");

    next_block = sb->first_data_block;

    /* Directory data first so children already have inode numbers. */
    for (uint32_t i = 0; i < node_count; i++)
        if (nodes[i].type == JEXFS3_DIRENT_DIR)
            fill_dir_data(&nodes[i]);

    for (uint32_t i = 0; i < node_count; i++) {
        struct node *n = &nodes[i];

        if (n->type == JEXFS3_DIRENT_DIR) {
            uint32_t subdirs = 0;
            for (uint32_t j = 0; j < node_count; j++)
                if (j != i && nodes[j].parent == i &&
                    nodes[j].type == JEXFS3_DIRENT_DIR)
                    subdirs++;
            n->links = 2 + subdirs;
            printf("  %-24s <dir>   %u block(s)\n", n->rel[0] ? n->rel : "/",
                   n->data_blocks);
        } else {
            n->links = 1;
            fill_file_data(n);
            printf("  %-24s %6llu bytes  %u block(s), %u extent(s)%s\n",
                   n->rel, (unsigned long long)n->size, n->data_blocks,
                   n->ext_count, n->ext_depth ? " (leaf)" : "");
        }
    }

    /* Inode table. */
    for (uint32_t i = 0; i < node_count; i++) {
        struct node *n = &nodes[i];
        struct jexfs3_inode *inode = inode_ptr(n->inode);

        memset(inode, 0, JEXFS3_INODE_SIZE);
        inode->mode = (uint16_t)((n->type == JEXFS3_DIRENT_DIR
                                  ? JEXFS3_TYPE_DIR | JEXFS3_IRUSR | JEXFS3_IWUSR
                                    | JEXFS3_IXUSR | JEXFS3_IRGRP | JEXFS3_IXGRP
                                    | JEXFS3_IROTH | JEXFS3_IXOTH
                                  : JEXFS3_TYPE_FILE | JEXFS3_IRUSR | JEXFS3_IWUSR
                                    | JEXFS3_IRGRP | JEXFS3_IROTH));
        inode->links = (uint16_t)n->links;
        inode->generation = 1;
        inode->size = n->size;
        inode->blocks = n->data_blocks + (n->ext_root ? 1 : 0);
        inode->data_crc = n->data_crc;
        inode->extent_count = (uint16_t)n->ext_count;
        inode->extent_depth = n->ext_depth;
        inode->extent_root = n->ext_root;
        if (n->ext_count <= JEXFS3_INLINE_EXTENTS)
            memcpy(inode->inline_extents, n->inline_ext,
                   n->ext_count * sizeof(struct jexfs3_extent));

        inode->checksum = jexfs3_crc_zeroed(inode, JEXFS3_INODE_SIZE,
                                            JEXFS3_INODE_CHECKSUM_OFF);
        inode_bitmap_set(n->inode);
    }

    /* Inode numbers 0 and 1 are reserved; root is 2 and the bitmap keeps
     * the reserved bits clear so a scanner never expects inode records
     * that were never written. */

    /* Blocks 0..next_block-1 are metadata and data; the backup block is
     * reserved separately at the end. */
    for (uint32_t b = 0; b < next_block; b++)
        block_bitmap_set(b);
    block_bitmap_set(total_blocks - 1);

    /* Superblock. */
    compute_uuid(sb->uuid);
    sb->magic = JEXFS3_MAGIC;
    sb->version = JEXFS3_VERSION;
    sb->block_size = JEXFS3_BLOCK_SIZE;
    sb->total_blocks = total_blocks;
    sb->total_bytes = (uint64_t)total_blocks * JEXFS3_BLOCK_SIZE;
    sb->generation = 1;
    sb->feature_incompat = JEXFS3_FEATURE_INCOMPAT_EXTENTS
                           | JEXFS3_FEATURE_INCOMPAT_CRC32C;
    sb->inode_count = inode_count;
    sb->inode_size = JEXFS3_INODE_SIZE;
    sb->journal_start = 0;
    sb->journal_blocks = 0;
    sb->root_inode = JEXFS3_ROOT_INODE;
    sb->checksum_type = 1;
    sb->backup_block = total_blocks - 1;
    sb->backup_generation = sb->generation;
    memcpy(sb->label, "JexOS rootfs", 12);

    sb->checksum = jexfs3_crc_zeroed(sb, JEXFS3_BLOCK_SIZE,
                                     JEXFS3_SB_CHECKSUM_OFF);
    memcpy(block_ptr(total_blocks - 1), sb, JEXFS3_BLOCK_SIZE);

    FILE *out = fopen(argv[1], "wb");
    if (!out)
        die("cannot open output image");
    if (fwrite(img, 1, image_size, out) != image_size) {
        fclose(out);
        die("short write on output image");
    }
    fclose(out);

    printf("Done: %u entries, %u inodes, %u data/extent blocks used\n",
           node_count, inode_count, next_block - sb->first_data_block);
    free(img);
    return 0;
}
