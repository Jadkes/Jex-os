/**
 * @file mkjexfs.c
 * @brief Host tool to create a JexFS v2 disk image.
 *
 * Runs on the build host (Linux). Packs the rootfs/ directory into a
 * JexFS v2 disk image with the on-disk layout specified in jexfs.h:
 *
 *   Block 0:       Reserved (boot block)
 *   Block 1:       Superblock
 *   Block 2:       Inode bitmap (1 block = 8192 inode slots)
 *   Blocks 3-10:   Block bitmap (8 blocks = 65536 bits)
 *   Blocks 11-53:  Inode table (43 blocks, ~1032 inodes)
 *   Block 54+:     Data blocks
 *
 * Usage: ./mkjexfs <output.img> [block_count]
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <errno.h>
#include "../src/include/jexfs.h"

/* Layout constants -- must match jexfs.h comments exactly */
#define SYS_BLOCKS              54       /* blocks 0-53 are metadata */
#define INODE_TABLE_START       11
#define DATA_START              54
#define DEFAULT_BLOCK_COUNT     4096
#define MAX_FILE_BLOCKS         JEXFS_DIRECT_COUNT    /* 8 direct blocks only */

/*
 * Inodes per block and total: 1024/42 = 24 (16 bytes spare per block).
 * 43 blocks * 24 inodes/block = 1032 inodes.
 */
#define INODES_PER_BLOCK        (BLOCK_SIZE / JEXFS_INODE_SIZE)
#define TOTAL_INODE_BLOCKS      43
#define TOTAL_INODES            (INODES_PER_BLOCK * TOTAL_INODE_BLOCKS)

/** Global image buffer. */
static uint8_t *img;

/** Total blocks in the image (set from argv). */
static uint32_t total_blocks;

/* ------------------------------------------------------------------ */
/*  Bitmap helpers                                                     */
/* ------------------------------------------------------------------ */

static void bitmap_set(uint32_t block_start, uint32_t index)
{
    uint32_t byte_off = index / 8;
    uint8_t  bit      = 1U << (index % 8);
    img[block_start * BLOCK_SIZE + byte_off] |= bit;
}

static void inode_bitmap_set(uint32_t idx)  { bitmap_set(2, idx); }
static void block_bitmap_set(uint32_t idx)  { bitmap_set(3, idx); }

/* ------------------------------------------------------------------ */
/*  Superblock helpers                                                 */
/* ------------------------------------------------------------------ */

static struct jex_superblock *superblock(void)
{
    return (struct jex_superblock *)(img + BLOCK_SIZE);
}

static struct jex_inode *inode_ptr(uint32_t idx)
{
    return (struct jex_inode *)(
        img + INODE_TABLE_START * BLOCK_SIZE + idx * JEXFS_INODE_SIZE);
}

static void inode_init(struct jex_inode *inode, uint16_t mode)
{
    memset(inode, 0, sizeof(*inode));
    inode->mode   = mode;
    inode->size   = 0;
    inode->uid    = 0;
    inode->gid    = 0;
    inode->atime  = 0;
    inode->mtime  = 0;
    inode->ctime  = 0;
    inode->indirect_block    = 0;
    inode->double_indirect   = 0;
}

/* ------------------------------------------------------------------ */
/*  Directory entry helpers (variable-length)                          */
/* ------------------------------------------------------------------ */

/**
 * Write a variable-length directory entry into the buffer at @p offset.
 * Returns the new offset (past the written entry).
 */
static uint32_t dirent_write(uint8_t *buf, uint32_t offset,
                             uint16_t inode, const char *name,
                             uint8_t file_type)
{
    struct jex_dir_entry *de = (struct jex_dir_entry *)(buf + offset);
    uint16_t name_len        = (uint16_t)strlen(name);

    de->inode     = inode;
    de->name_len  = name_len;
    de->file_type = file_type;
    memcpy(de->name, name, name_len);
    return offset + sizeof(*de) + name_len;
}

/* ------------------------------------------------------------------ */
/*  Main                                                               */
/* ------------------------------------------------------------------ */

int main(int argc, char **argv)
{
    uint32_t next_inode      = 2;    /* 0=null, 1=root */
    uint32_t next_data_block = DATA_START + 1;
    uint32_t entry_count     = 2;    /* ., .. */

    /* ---- parse arguments ---- */
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <output.img> [block_count]\n", argv[0]);
        return 1;
    }
    total_blocks = (argc > 2) ? (uint32_t)atoi(argv[2]) : DEFAULT_BLOCK_COUNT;
    if (total_blocks < 256) {
        fprintf(stderr, "Error: block_count must be at least 256\n");
        return 1;
    }

    uint32_t image_size = total_blocks * BLOCK_SIZE;
    img = (uint8_t *)calloc(1, image_size);
    if (!img) {
        fprintf(stderr, "Error: calloc(%u) failed\n", image_size);
        return 1;
    }

    printf("JexFS v2: %s  blocks=%u  size=%u bytes\n",
           argv[1], total_blocks, image_size);

    /* ---- superblock (block 1) ---- */
    struct jex_superblock *sb = superblock();
    sb->magic             = JEXFS_MAGIC;           /* 0x4A455846 */
    sb->total_blocks      = total_blocks;
    sb->total_inodes      = TOTAL_INODES;          /* 1032 */
    sb->inode_bitmap_start = 2;
    sb->block_bitmap_start = 3;
    sb->bitmap_blocks      = 8;                    /* blocks 3-10 */
    sb->inode_table_start  = INODE_TABLE_START;    /* 11 */
    sb->inode_size         = JEXFS_INODE_SIZE;      /* 42 */
    sb->data_start         = DATA_START;            /* 54 */

    /* ---- reserve metadata blocks (0-53) in block bitmap ---- */
    for (uint32_t i = 0; i < SYS_BLOCKS; i++)
        block_bitmap_set(i);

    /* ---- reserve inode 0 (unused) and inode 1 (root) ---- */
    inode_bitmap_set(0);
    inode_bitmap_set(1);

    /* ---- root inode (inode 1) ---- */
    struct jex_inode *root_inode = inode_ptr(1);
    inode_init(root_inode,
               JEXFS_TYPE_DIR |
               JEXFS_IRUSR | JEXFS_IWUSR | JEXFS_IXUSR |
               JEXFS_IRGRP | JEXFS_IXGRP |
               JEXFS_IROTH | JEXFS_IXOTH);
    root_inode->direct_blocks[0] = DATA_START;
    block_bitmap_set(DATA_START);

    /* ---- root directory entries (variable-length) ---- */
    uint8_t *dir_data  = img + DATA_START * BLOCK_SIZE;
    uint32_t dir_off   = 0;

    dir_off = dirent_write(dir_data, dir_off, 1, ".",  JEXFS_DIRENT_DIR);
    dir_off = dirent_write(dir_data, dir_off, 1, "..", JEXFS_DIRENT_DIR);
    root_inode->size = dir_off;

    /* ---- pack files from rootfs/ ---- */
    DIR *d = opendir("rootfs");
    if (!d) {
        fprintf(stderr, "Warning: cannot open rootfs/ (%s)\n", strerror(errno));
    } else {
        struct dirent *ent;
        while ((ent = readdir(d)) != NULL) {
            if (ent->d_name[0] == '.')
                continue;

            /* Build source path */
            char path[512];
            int ret = snprintf(path, sizeof(path), "rootfs/%s", ent->d_name);
            if (ret < 0 || (size_t)ret >= sizeof(path)) {
                fprintf(stderr, "Warning: path too long for %s\n", ent->d_name);
                continue;
            }

            FILE *f = fopen(path, "rb");
            if (!f) {
                fprintf(stderr, "Warning: cannot open %s (%s)\n",
                        path, strerror(errno));
                continue;
            }

            /* File size */
            if (fseek(f, 0, SEEK_END) != 0) {
                fprintf(stderr, "Warning: fseek failed on %s\n", path);
                fclose(f);
                continue;
            }
            long file_size = ftell(f);
            if (file_size < 0) {
                fprintf(stderr, "Warning: ftell failed on %s\n", path);
                fclose(f);
                continue;
            }
            rewind(f);

            uint32_t size = (uint32_t)file_size;

            /* Clamp to direct-block limit (8 blocks = 8192 bytes) */
            if (size > MAX_FILE_BLOCKS * BLOCK_SIZE) {
                fprintf(stderr, "Warning: %s is %lu bytes, truncating to %u\n",
                        ent->d_name, (unsigned long)file_size,
                        MAX_FILE_BLOCKS * BLOCK_SIZE);
                size = MAX_FILE_BLOCKS * BLOCK_SIZE;
            }

            uint32_t blocks_needed = (size + BLOCK_SIZE - 1) / BLOCK_SIZE;

            /* Check resource limits */
            if (next_inode >= TOTAL_INODES) {
                fprintf(stderr, "Error: out of inodes (max %u)\n", TOTAL_INODES);
                fclose(f);
                break;
            }
            if (next_data_block + blocks_needed > total_blocks) {
                fprintf(stderr, "Error: image full at block %u (max %u)\n",
                        next_data_block, total_blocks);
                fclose(f);
                break;
            }

            /* Allocate inode */
            inode_bitmap_set(next_inode);
            struct jex_inode *inode = inode_ptr(next_inode);
            inode_init(inode,
                       JEXFS_TYPE_FILE |
                       JEXFS_IRUSR | JEXFS_IWUSR |
                       JEXFS_IRGRP | JEXFS_IROTH);
            inode->size = size;

            /* Copy file data into direct blocks */
            for (uint32_t b = 0; b < blocks_needed; b++) {
                inode->direct_blocks[b] = (uint16_t)next_data_block;
                block_bitmap_set(next_data_block);
                size_t nread = fread(img + next_data_block * BLOCK_SIZE,
                                     1, BLOCK_SIZE, f);
                if (nread == 0 && ferror(f)) {
                    fprintf(stderr, "Warning: read error on %s\n", path);
                    break;
                }
                next_data_block++;
            }

            /* Add directory entry to root dir */
            if (dir_off + sizeof(struct jex_dir_entry) + strlen(ent->d_name)
                > BLOCK_SIZE) {
                fprintf(stderr, "Warning: root directory full, skipping %s\n",
                        ent->d_name);
                fclose(f);
                break;
            }

            dir_off = dirent_write(dir_data, dir_off,
                                   (uint16_t)next_inode, ent->d_name,
                                   JEXFS_DIRENT_FILE);
            root_inode->size = dir_off;
            next_inode++;
            entry_count++;

            printf("  %-20s %5u bytes  %u block(s)\n",
                   ent->d_name, size, blocks_needed);
            fclose(f);
        }
        closedir(d);
    }

    /* ---- write image to disk ---- */
    FILE *out = fopen(argv[1], "wb");
    if (!out) {
        fprintf(stderr, "Error: cannot open %s for writing (%s)\n",
                argv[1], strerror(errno));
        free(img);
        return 1;
    }

    size_t written = fwrite(img, 1, image_size, out);
    if (written != image_size) {
        fprintf(stderr, "Error: wrote %zu of %u bytes\n", written, image_size);
        fclose(out);
        free(img);
        return 1;
    }
    fclose(out);

    printf("Done: %u entries, %u inodes used, %u data blocks written\n",
           entry_count, next_inode, next_data_block - DATA_START);
    free(img);
    return 0;
}
