/**
 * @file jexfs.c
 * @brief JexFS v2 -- Kernel-mode filesystem driver.
 *
 * Block I/O, inode management, indirect block traversal (single + double),
 * bitmap allocator spanning 8 blocks, variable-length directory operations.
 *
 * Layout:
 *   Block 0:  Reserved
 *   Block 1:  Superblock
 *   Block 2:  Inode bitmap (8192 slots)
 *   Blocks 3-10: Block bitmap (65536 bits)
 *   Blocks 11-53: Inode table (43 blocks, 1024 x 42-byte inodes)
 *   Block 54+: Data blocks
 */

#define pr_fmt(fmt) "[JEXFS] " fmt
#include "jexfs.h"
#include "ide.h"
#include "multiboot.h"
#include "kernel/printk.h"
#include "string.h"
#include "terminal.h"
#include <jexos/errno.h>
#include "kernel/mutex.h"
#include "pmm.h"

extern multiboot_info_t *g_mboot_info;

/*
 * RAM-backed root image.  When a bootloader (GRUB) loads the JexFS image
 * into memory as a multiboot module, block I/O is served from RAM instead
 * of the IDE disk.  This lets JexOS boot from a single CD/floppy with no
 * attached IDE disk.  NULL means "use the IDE primary master".
 */
static uint8_t *ramdisk_base;
static uint32_t ramdisk_blocks;

/*
 * Disk root: LBA of the sector holding block 0 when the mounted rootfs
 * lives on the primary master.  Zero for a raw image at the start of the
 * drive; the tail of a delivery image otherwise.  ramdisk_base == NULL
 * selects the disk path.
 */
static uint32_t disk_root_lba;

/*
 * Page-aligned extent of the multiboot module that carried the RAM
 * image.  Once the disk mount replaces it, these frames go back to the
 * PMM so a 64 MB image stops costing half of guest RAM.
 */
static uint32_t module_first_frame;
static uint32_t module_frame_count;

/* ── Filesystem lock ───────────────────────────────────
 * One coarse sleeping lock covers every metadata operation.  JexFS ops
 * are RAM-speed so contention is brief, and a coarse lock beats a web
 * of fine-grained ones in a hobby kernel.  All *_nolock functions are
 * unsafe to call directly from outside this file. */
static mutex_t fs_lock;

/** @brief Take the filesystem lock (for multi-step caller sequences). */
void jexfs_lock(void)   { mutex_lock(&fs_lock); }

/** @brief Release the filesystem lock. */
void jexfs_unlock(void) { mutex_unlock(&fs_lock); }


void jexfs_set_ramdisk(void *base, uint32_t bytes)
{
    ramdisk_base   = (uint8_t *)base;
    ramdisk_blocks = (bytes + BLOCK_SIZE - 1) / BLOCK_SIZE;
}

static inline int jfs_memcmp(const void *a, const void *b, uint32_t len) {
    const unsigned char *ca = (const unsigned char *)a;
    const unsigned char *cb = (const unsigned char *)b;
    for (uint32_t i = 0; i < len; i++)
        if (ca[i] != cb[i]) return (int)(ca[i]) - (int)(cb[i]);
    return 0;
}

static struct jex_superblock sb;
static int jexfs_mounted = 0;
uint32_t cwd_inode = 1;

static int jexfs_add_dirent(uint32_t dir_ino, uint16_t entry_ino,
                            const char *name, uint16_t name_len, uint8_t file_type);


void read_block(uint32_t block, uint8_t *buffer)
{
    if (ramdisk_base) {
        if (block >= ramdisk_blocks) {
            pr_serial("jexfs: read_block out of range block=%u\n", block);
            memset(buffer, 0, BLOCK_SIZE);
            return;
        }
        memcpy(buffer, ramdisk_base + block * BLOCK_SIZE, BLOCK_SIZE);
        return;
    }
    uint32_t lba = disk_root_lba + block * 2;
    ide_read_sector(lba, buffer);
    ide_read_sector(lba + 1, buffer + 512);
}

void write_block(uint32_t block, const uint8_t *buffer)
{
    if (ramdisk_base) {
        if (block >= ramdisk_blocks) {
            pr_serial("jexfs: write_block out of range block=%u\n", block);
            return;
        }
        memcpy(ramdisk_base + block * BLOCK_SIZE, buffer, BLOCK_SIZE);
        return;
    }
    uint32_t lba = disk_root_lba + block * 2;
    ide_write_sector(lba, buffer);
    ide_write_sector(lba + 1, buffer + 512);
}

/**
 * jexfs_alloc_block -- Allocate a free data block from the bitmap.
 * @return Block number on success, -ENOSPC if none free.
 */
static int jexfs_alloc_block(void)
{
    uint8_t bmap_buf[BLOCK_SIZE];
    int last_read = -1;

    for (uint32_t i = sb.data_start; i < sb.total_blocks; i++) {
        uint32_t bm_blk = sb.block_bitmap_start + i / (BLOCK_SIZE * 8);
        if (bm_blk >= sb.block_bitmap_start + sb.bitmap_blocks)
            break;

        if ((int)bm_blk != last_read) {
            read_block(bm_blk, bmap_buf);
            last_read = (int)bm_blk;
        }

        uint32_t bit = i % (BLOCK_SIZE * 8);
        if (!(bmap_buf[bit / 8] & (1u << (bit % 8)))) {
            bmap_buf[bit / 8] |= (1u << (bit % 8));
            write_block(bm_blk, bmap_buf);
            return (int)i;
        }
    }
    return -ENOSPC;
}

static void jexfs_free_block(uint32_t block)
{
    if (block == 0) return;
    uint32_t bm_blk = sb.block_bitmap_start + block / (BLOCK_SIZE * 8);
    uint32_t bit     = block % (BLOCK_SIZE * 8);
    uint8_t buf[BLOCK_SIZE];
    read_block(bm_blk, buf);
    buf[bit / 8] &= ~(1u << (bit % 8));
    write_block(bm_blk, buf);
}

static int jexfs_alloc_inode(void)
{
    uint8_t buf[BLOCK_SIZE];
    read_block(sb.inode_bitmap_start, buf);
    for (uint32_t i = 1; i < sb.total_inodes; i++) {
        if (!(buf[i / 8] & (1u << (i % 8)))) {
            buf[i / 8] |= (1u << (i % 8));
            write_block(sb.inode_bitmap_start, buf);
            return (int)i;
        }
    }
    return -ENOSPC;
}

static void jexfs_free_inode(uint32_t idx)
{
    if (idx == 0) return;
    uint8_t buf[BLOCK_SIZE];
    read_block(sb.inode_bitmap_start, buf);
    buf[idx / 8] &= ~(1u << (idx % 8));
    write_block(sb.inode_bitmap_start, buf);
}

/**
 * @brief Copy an inode without taking fs_lock.
 * Caller MUST hold jexfs_lock(); exists so multi-step walkers (ls,
 * completion) can scan consistently instead of locking per step.
 */
void jexfs_read_inode_raw(uint32_t idx, struct jex_inode *inode)
{
    if (idx >= sb.total_inodes) {
        memset(inode, 0, sizeof(*inode));
        return;
    }
    uint8_t buf[BLOCK_SIZE];
    uint32_t block = sb.inode_table_start + (idx * JEXFS_INODE_SIZE) / BLOCK_SIZE;
    uint32_t offset = (idx * JEXFS_INODE_SIZE) % BLOCK_SIZE;
    read_block(block, buf);
    memcpy(inode, buf + offset, JEXFS_INODE_SIZE);
}

static void write_inode_nolock(uint32_t idx, struct jex_inode *inode)
{
    if (idx >= sb.total_inodes) return;
    uint8_t buf[BLOCK_SIZE];
    uint32_t block = sb.inode_table_start + (idx * JEXFS_INODE_SIZE) / BLOCK_SIZE;
    uint32_t offset = (idx * JEXFS_INODE_SIZE) % BLOCK_SIZE;
    read_block(block, buf);
    memcpy(buf + offset, inode, JEXFS_INODE_SIZE);
    write_block(block, buf);
}

static void jexfs_stat_nolock(int inode_idx, struct jex_inode *inode)
{
    if (!jexfs_mounted) { memset(inode, 0, sizeof(*inode)); return; }
    jexfs_read_inode_raw((uint32_t)inode_idx, inode);
}

/**
 * jexfs_bmap -- Translate logical block index to physical block number.
 * @return Physical block number, or 0 if not allocated.
 *
 *   Direct:   lblocks 0-7
 *   Indirect: lblocks 8-519  (512 entries via 1 indirect block)
 *   Dbl-ind:  lblocks 520+  (512x512 entries via double indirect)
 */
static uint32_t jexfs_bmap(struct jex_inode *inode, uint32_t lblock)
{
    uint16_t epb = JEXFS_INDIRECT_ENTRIES;  /* 512 */
    uint8_t buf[BLOCK_SIZE];

    if (lblock < JEXFS_DIRECT_COUNT)
        return inode->direct_blocks[lblock];

    lblock -= JEXFS_DIRECT_COUNT;
    if (lblock < epb) {
        if (inode->indirect_block == 0) return 0;
        read_block(inode->indirect_block, buf);
        return ((uint16_t *)buf)[lblock];
    }

    lblock -= epb;
    if (inode->double_indirect == 0) return 0;
    uint32_t i1 = lblock / epb, i2 = lblock % epb;
    if (i1 >= epb) return 0;

    read_block(inode->double_indirect, buf);
    uint16_t *l1 = (uint16_t *)buf;
    if (l1[i1] == 0) return 0;
    read_block(l1[i1], buf);
    return ((uint16_t *)buf)[i2];
}

/**
 * jexfs_bmap_alloc -- Like bmap but allocates blocks (and intermediate
 * indirect blocks) as needed.
 * @return Physical block number, or negative errno.
 */
static int jexfs_bmap_alloc(struct jex_inode *inode, uint32_t lblock)
{
    uint32_t exist = jexfs_bmap(inode, lblock);
    if (exist != 0) return (int)exist;

    uint16_t epb = JEXFS_INDIRECT_ENTRIES;
    uint8_t buf[BLOCK_SIZE];
    int nb;

    /* Direct */
    if (lblock < JEXFS_DIRECT_COUNT) {
        nb = jexfs_alloc_block();
        if (nb < 0) return nb;
        inode->direct_blocks[lblock] = (uint16_t)nb;
        return nb;
    }

    /* Single indirect */
    uint32_t adj = lblock - JEXFS_DIRECT_COUNT;
    if (adj < epb) {
        if (inode->indirect_block == 0) {
            nb = jexfs_alloc_block();
            if (nb < 0) return nb;
            inode->indirect_block = (uint16_t)nb;
            memset(buf, 0, BLOCK_SIZE);
            write_block((uint32_t)nb, buf);
        }
        read_block(inode->indirect_block, buf);
        uint16_t *tbl = (uint16_t *)buf;
        if (tbl[adj] == 0) {
            nb = jexfs_alloc_block();
            if (nb < 0) return nb;
            tbl[adj] = (uint16_t)nb;
            write_block(inode->indirect_block, buf);
        }
        return tbl[adj];
    }

    /* Double indirect */
    adj -= epb;
    uint32_t i1 = adj / epb, i2 = adj % epb;
    if (i1 >= epb) return -EFBIG;

    if (inode->double_indirect == 0) {
        nb = jexfs_alloc_block();
        if (nb < 0) return nb;
        inode->double_indirect = (uint16_t)nb;
        memset(buf, 0, BLOCK_SIZE);
        write_block((uint32_t)nb, buf);
    }

    read_block(inode->double_indirect, buf);
    uint16_t *l1 = (uint16_t *)buf;
    if (l1[i1] == 0) {
        nb = jexfs_alloc_block();
        if (nb < 0) return nb;
        l1[i1] = (uint16_t)nb;
        write_block(inode->double_indirect, buf);
        memset(buf, 0, BLOCK_SIZE);
        write_block((uint32_t)nb, buf);
    } else {
        read_block(l1[i1], buf);
    }

    uint16_t *l2 = (uint16_t *)buf;
    if (l2[i2] == 0) {
        nb = jexfs_alloc_block();
        if (nb < 0) return nb;
        l2[i2] = (uint16_t)nb;
        write_block(l1[i1], buf);
    }
    return l2[i2];
}

/**
 * jexfs_find_entry -- Look up a name in a directory inode.
 * @return Inode index on success, -ENOENT if not found.
 */
static int jexfs_find_entry(uint32_t dir_ino, const char *name, uint16_t name_len)
{
    struct jex_inode di;
    jexfs_read_inode_raw(dir_ino, &di);
    if ((di.mode & JEXFS_TYPE_MASK) != JEXFS_TYPE_DIR)
        return -ENOTDIR;

    uint8_t buf[BLOCK_SIZE];
    uint32_t off = 0;

    while (off + 5 <= di.size) {
        uint32_t lb = off / BLOCK_SIZE, bo = off % BLOCK_SIZE;
        if (bo >= BLOCK_SIZE) { off += BLOCK_SIZE - bo; continue; }

        uint32_t phys = jexfs_bmap(&di, lb);
        if (phys == 0) break;

        read_block(phys, buf);
        while (bo + 5 <= BLOCK_SIZE && off < di.size) {
            struct jex_dir_entry *de = (struct jex_dir_entry *)(buf + bo);
            /* Zeroed bytes within di.size are holes (padding or tombstones),
             * not end-of-directory -- di.size is the only authoritative end */
            if (de->inode == 0 && de->name_len == 0) {
                bo += 5; off += 5;
                continue;
            }
            if (de->inode != 0 && de->name_len == name_len
                && jfs_memcmp(de->name, name, name_len) == 0)
                return (int)de->inode;
            uint16_t esz = 5 + de->name_len;
            bo += esz; off += esz;
        }
        /* Inner walk stopped mid-block or at its edge.  Advance to the
         * next block boundary so the outer loop always makes progress --
         * but only when not already on one: an entry that ends exactly at
         * the boundary would otherwise skip the whole next block, losing
         * every name stored after it. */
        if (off % BLOCK_SIZE)
            off += BLOCK_SIZE - (off % BLOCK_SIZE);
    }
    return -ENOENT;
}

static int open_nolock(const char *name)
{
    if (!jexfs_mounted) return -ENODEV;
    if (!name || name[0] == '\0') return (int)cwd_inode;
    uint32_t dir = (name[0] == '/') ? 1 : cwd_inode;
    if (name[0] == '/') name++;
    if (name[0] == '\0') return 1;
    return jexfs_find_entry(dir, name, (uint16_t)strlen(name));
}

static int jexfs_read_nolock(int inode_idx, void *buffer, uint32_t size, uint32_t offset)
{
    if (!jexfs_mounted) return -ENODEV;
    struct jex_inode inode;
    jexfs_read_inode_raw((uint32_t)inode_idx, &inode);
    if (offset >= inode.size) return 0;
    if (offset + size > inode.size) size = inode.size - offset;

    uint32_t remain = size, curr = offset;
    uint8_t *out = (uint8_t *)buffer;

    while (remain > 0) {
        uint32_t lb = curr / BLOCK_SIZE, bo = curr % BLOCK_SIZE;
        uint32_t chunk = BLOCK_SIZE - bo;
        if (chunk > remain) chunk = remain;

        uint32_t phys = jexfs_bmap(&inode, lb);
        if (phys == 0) break;

        uint8_t buf[BLOCK_SIZE];
        read_block(phys, buf);
        memcpy(out, buf + bo, chunk);
        remain -= chunk; out += chunk; curr += chunk;
    }
    return (int)(size - remain);
}

static int jexfs_write_nolock(int inode_idx, const void *buffer,
                uint32_t size, uint32_t offset)
{
    if (!jexfs_mounted) return -ENODEV;
    struct jex_inode inode;
    jexfs_read_inode_raw((uint32_t)inode_idx, &inode);

    uint32_t remain = size, curr = offset;
    const uint8_t *in = (const uint8_t *)buffer;

    while (remain > 0) {
        uint32_t lb = curr / BLOCK_SIZE, bo = curr % BLOCK_SIZE;
        uint32_t chunk = BLOCK_SIZE - bo;
        if (chunk > remain) chunk = remain;

        int phys = jexfs_bmap_alloc(&inode, lb);
        if (phys < 0) {
            if (curr > offset) {
                inode.size = (inode.size > curr) ? inode.size : curr;
                write_inode_nolock((uint32_t)inode_idx, &inode);
            }
            return phys;
        }

        uint8_t buf[BLOCK_SIZE];
        read_block((uint32_t)phys, buf);
        memcpy(buf + bo, in, chunk);
        write_block((uint32_t)phys, buf);
        remain -= chunk; in += chunk; curr += chunk;
    }

    if (offset + size > inode.size)
        inode.size = offset + size;
    write_inode_nolock((uint32_t)inode_idx, &inode);
    return (int)(size - remain);
}

static int jexfs_create_nolock(const char *name)
{
    if (!jexfs_mounted) return -ENODEV;
    if (!name || name[0] == '\0') return -EINVAL;
    uint16_t nl = (uint16_t)strlen(name);
    if (nl > 255) return -ENAMETOOLONG;

    int existing = open_nolock(name);
    if (existing >= 0) return existing;

    int ino = jexfs_alloc_inode();
    if (ino < 0) return ino;

    struct jex_inode inode;
    memset(&inode, 0, sizeof(inode));
    inode.mode = JEXFS_TYPE_FILE | JEXFS_IRUSR | JEXFS_IWUSR
                 | JEXFS_IRGRP | JEXFS_IROTH;
    write_inode_nolock((uint32_t)ino, &inode);

    int ret = jexfs_add_dirent(cwd_inode, (uint16_t)ino, name, nl, JEXFS_DIRENT_FILE);
    if (ret < 0) { jexfs_free_inode((uint32_t)ino); return ret; }
    return ino;
}

static int jexfs_mkdir_nolock(const char *name)
{
    if (!jexfs_mounted) return -ENODEV;
    if (!name || name[0] == '\0') return -EINVAL;
    uint16_t nl = (uint16_t)strlen(name);
    if (nl > 255) return -ENAMETOOLONG;

    int ino = jexfs_alloc_inode();
    if (ino < 0) return ino;

    int blk = jexfs_alloc_block();
    if (blk < 0) { jexfs_free_inode((uint32_t)ino); return blk; }

    /* Initialize directory block: . and .. */
    uint8_t buf[BLOCK_SIZE];
    memset(buf, 0, BLOCK_SIZE);
    int off = 0;
    struct jex_dir_entry *de;

    de = (struct jex_dir_entry *)(buf + off);
    de->inode = (uint16_t)ino; de->name_len = 1; de->file_type = JEXFS_DIRENT_DIR;
    de->name[0] = '.';  off += 6;

    de = (struct jex_dir_entry *)(buf + off);
    de->inode = (uint16_t)cwd_inode; de->name_len = 2; de->file_type = JEXFS_DIRENT_DIR;
    de->name[0] = '.'; de->name[1] = '.';  off += 7;

    write_block((uint32_t)blk, buf);

    struct jex_inode inode;
    memset(&inode, 0, sizeof(inode));
    inode.mode = JEXFS_TYPE_DIR | JEXFS_IRUSR | JEXFS_IWUSR | JEXFS_IXUSR
                 | JEXFS_IRGRP | JEXFS_IXGRP | JEXFS_IROTH | JEXFS_IXOTH;
    inode.size = (uint32_t)off;
    inode.direct_blocks[0] = (uint16_t)blk;
    write_inode_nolock((uint32_t)ino, &inode);

    int ret = jexfs_add_dirent(cwd_inode, (uint16_t)ino, name, nl, JEXFS_DIRENT_DIR);
    if (ret < 0) { jexfs_free_inode((uint32_t)ino); jexfs_free_block((uint32_t)blk); }
    return ret;
}

/**
 * jexfs_add_dirent -- Add a directory entry to a directory inode.
 *
 * Scans existing blocks for a reusable slot (deleted entry large enough,
 * or end-of-directory marker). If none found, allocates a new data block.
 */
static int jexfs_add_dirent(uint32_t dir_ino, uint16_t entry_ino,
                            const char *name, uint16_t name_len,
                            uint8_t file_type)
{
    struct jex_inode di;
    jexfs_read_inode_raw(dir_ino, &di);
    if ((di.mode & JEXFS_TYPE_MASK) != JEXFS_TYPE_DIR)
        return -ENOTDIR;

    int esz = 5 + name_len;
    uint8_t buf[BLOCK_SIZE];
    uint32_t off = 0;
    int found_off = -1;

    /* Scan for reusable slot */
    while (off + 5 <= di.size) {
        uint32_t lb = off / BLOCK_SIZE, bo = off % BLOCK_SIZE;
        if (bo + esz > BLOCK_SIZE) { off += BLOCK_SIZE - bo; continue; }

        uint32_t phys = jexfs_bmap(&di, lb);
        if (phys == 0) { off += BLOCK_SIZE - bo; continue; }

        read_block(phys, buf);
        while (bo + 5 <= BLOCK_SIZE && off < di.size) {
            struct jex_dir_entry *de = (struct jex_dir_entry *)(buf + bo);
            if (de->inode == 0 && de->name_len == 0) {
                /* Zeroed gap: only take it if the new name fits the block */
                if (bo + 5 + name_len <= BLOCK_SIZE) {
                    found_off = (int)off; goto write_entry;
                }
            }
            if (de->inode == 0 && de->name_len >= name_len) {
                found_off = (int)off; goto write_entry;
            }
            uint16_t skip = 5 + de->name_len;
            bo += skip; off += skip;
        }
    }

    /* No slot found -- allocate a new data block */
    if (found_off < 0) {
        int nb = jexfs_alloc_block();
        if (nb < 0) return nb;

        /* Find next free logical block index */
        uint32_t lb = 0;
        while (jexfs_bmap(&di, lb) != 0) lb++;
        int r = jexfs_bmap_alloc(&di, lb);
        if (r < 0) { jexfs_free_block((uint32_t)nb); return r; }

        if (di.size < (lb + 1) * BLOCK_SIZE)
            di.size = (lb + 1) * BLOCK_SIZE;
        write_inode_nolock(dir_ino, &di);

        found_off = (int)(lb * BLOCK_SIZE);
        memset(buf, 0, BLOCK_SIZE);
        write_block((uint32_t)nb, buf);
    }

write_entry:
    {
        uint32_t lb = (uint32_t)found_off / BLOCK_SIZE;
        uint32_t bo = (uint32_t)found_off % BLOCK_SIZE;
        uint32_t phys = jexfs_bmap(&di, lb);
        if (phys == 0) return -EIO;
        if (bo + 5 + name_len > BLOCK_SIZE) return -EIO; /* never write past the block */

        read_block(phys, buf);
        struct jex_dir_entry *de = (struct jex_dir_entry *)(buf + bo);
        de->inode = entry_ino;
        de->name_len = name_len;
        de->file_type = file_type;
        memcpy(de->name, name, name_len);

        uint32_t new_end = (uint32_t)found_off + (uint32_t)esz;
        if (new_end > di.size) {
            di.size = new_end;
            write_inode_nolock(dir_ino, &di);
        }
        write_block(phys, buf);
    }
    return 0;
}

static int jexfs_remove_nolock(const char *name)
{
    if (!jexfs_mounted) return -ENODEV;
    if (!name || name[0] == '\0') return -EINVAL;
    uint16_t nl = (uint16_t)strlen(name);

    struct jex_inode di;
    jexfs_read_inode_raw(cwd_inode, &di);
    uint8_t buf[BLOCK_SIZE];
    uint32_t off = 0;

    while (off + 5 <= di.size) {
        uint32_t lb = off / BLOCK_SIZE, bo = off % BLOCK_SIZE;
        if (bo >= BLOCK_SIZE) { off += BLOCK_SIZE - bo; continue; }
        uint32_t phys = jexfs_bmap(&di, lb);
        if (phys == 0) break;

        read_block(phys, buf);
        while (bo + 5 <= BLOCK_SIZE && off < di.size) {
            struct jex_dir_entry *de = (struct jex_dir_entry *)(buf + bo);
            if (de->inode == 0 && de->name_len == 0) {
                bo += 5; off += 5;
                continue;
            }
            if (de->inode != 0 && de->name_len == nl
                && jfs_memcmp(de->name, name, nl) == 0) {
                /* Found -- free inode and blocks */
                struct jex_inode inode;
                jexfs_read_inode_raw(de->inode, &inode);

                /* Free direct blocks */
                for (int i = 0; i < JEXFS_DIRECT_COUNT; i++)
                    if (inode.direct_blocks[i] != 0)
                        jexfs_free_block(inode.direct_blocks[i]);

                /* Free indirect block entries + the indirect block itself */
                if (inode.indirect_block != 0) {
                    uint8_t tbuf[BLOCK_SIZE];
                    read_block(inode.indirect_block, tbuf);
                    uint16_t *t = (uint16_t *)tbuf;
                    for (uint32_t i = 0; i < JEXFS_INDIRECT_ENTRIES; i++)
                        if (t[i] != 0) jexfs_free_block(t[i]);
                    jexfs_free_block(inode.indirect_block);
                }

                /* Free double-indirect */
                if (inode.double_indirect != 0) {
                    uint8_t dbuf[BLOCK_SIZE];
                    read_block(inode.double_indirect, dbuf);
                    uint16_t *l1 = (uint16_t *)dbuf;
                    for (uint32_t i = 0; i < JEXFS_INDIRECT_ENTRIES; i++) {
                        if (l1[i] == 0) continue;
                        uint8_t l2b[BLOCK_SIZE];
                        read_block(l1[i], l2b);
                        uint16_t *l2 = (uint16_t *)l2b;
                        for (uint32_t j = 0; j < JEXFS_INDIRECT_ENTRIES; j++)
                            if (l2[j] != 0) jexfs_free_block(l2[j]);
                        jexfs_free_block(l1[i]);
                    }
                    jexfs_free_block(inode.double_indirect);
                }

                jexfs_free_inode(de->inode);
                de->inode = 0;  /* free entry, preserve name_len for skip */
                write_block(phys, buf);
                return 0;
            }
            uint16_t esz = 5 + de->name_len;
            bo += esz; off += esz;
        }
        /* Inner walk stopped mid-block or at its edge.  Advance to the
         * next block boundary so the outer loop always makes progress --
         * but only when not already on one: an entry that ends exactly at
         * the boundary would otherwise skip the whole next block, losing
         * every name stored after it. */
        if (off % BLOCK_SIZE)
            off += BLOCK_SIZE - (off % BLOCK_SIZE);
    }
    return -ENOENT;
}

static int jexfs_rename_nolock(const char *old_name, const char *new_name)
{
    if (!jexfs_mounted) return -ENODEV;
    if (!old_name || !new_name) return -EINVAL;
    uint16_t old_nl = (uint16_t)strlen(old_name);
    uint16_t new_nl = (uint16_t)strlen(new_name);

    struct jex_inode di;
    jexfs_read_inode_raw(cwd_inode, &di);
    uint8_t buf[BLOCK_SIZE];
    uint32_t off = 0;

    while (off + 5 <= di.size) {
        uint32_t lb = off / BLOCK_SIZE, bo = off % BLOCK_SIZE;
        if (bo >= BLOCK_SIZE) { off += BLOCK_SIZE - bo; continue; }
        uint32_t phys = jexfs_bmap(&di, lb);
        if (phys == 0) break;

        read_block(phys, buf);
        while (bo + 5 <= BLOCK_SIZE && off < di.size) {
            struct jex_dir_entry *de = (struct jex_dir_entry *)(buf + bo);
            if (de->inode == 0 && de->name_len == 0) return -ENOENT;
            if (de->inode != 0 && de->name_len == old_nl
                && jfs_memcmp(de->name, old_name, old_nl) == 0) {
                if (new_nl <= de->name_len) {
                    /* Fits in existing slot */
                    de->name_len = new_nl;
                    memcpy(de->name, new_name, new_nl);
                    write_block(phys, buf);
                    return 0;
                }
                /* Too big -- delete and recreate */
                uint16_t saved_ino = de->inode;
                de->inode = 0;
                write_block(phys, buf);
                int ret = jexfs_add_dirent(cwd_inode, saved_ino,
                                           new_name, new_nl, JEXFS_DIRENT_FILE);
                if (ret < 0) {
                    /* Put back */
                    de->inode = saved_ino;
                    de->name_len = old_nl;
                    memcpy(de->name, old_name, old_nl);
                    write_block(phys, buf);
                }
                return ret;
            }
            uint16_t esz = 5 + de->name_len;
            bo += esz; off += esz;
        }
        /* Inner walk stopped mid-block or at its edge.  Advance to the
         * next block boundary so the outer loop always makes progress --
         * but only when not already on one: an entry that ends exactly at
         * the boundary would otherwise skip the whole next block, losing
         * every name stored after it. */
        if (off % BLOCK_SIZE)
            off += BLOCK_SIZE - (off % BLOCK_SIZE);
    }
    return -ENOENT;
}

static int jexfs_get_size_nolock(int inode_idx)
{
    if (!jexfs_mounted) return 0;
    struct jex_inode inode;
    jexfs_read_inode_raw((uint32_t)inode_idx, &inode);
    return (int)inode.size;
}

static void jexfs_list_dir_nolock(uint32_t inode_idx)
{
    if (!jexfs_mounted) { terminal_writestring("(no JEXFS)\n"); return; }
    struct jex_inode di;
    jexfs_read_inode_raw(inode_idx, &di);
    if ((di.mode & JEXFS_TYPE_MASK) != JEXFS_TYPE_DIR) return;

    uint8_t buf[BLOCK_SIZE];
    uint32_t off = 0;

    while (off + 5 <= di.size) {
        uint32_t lb = off / BLOCK_SIZE, bo = off % BLOCK_SIZE;
        if (bo >= BLOCK_SIZE) { off += BLOCK_SIZE - bo; continue; }
        uint32_t phys = jexfs_bmap(&di, lb);
        if (phys == 0) break;

        read_block(phys, buf);
        while (bo + 5 <= BLOCK_SIZE && off < di.size) {
            struct jex_dir_entry *de = (struct jex_dir_entry *)(buf + bo);
            if (de->inode == 0 && de->name_len == 0) {
                bo += 5; off += 5;
                continue;
            }
            if (de->inode != 0) {
                /* Skip . and .. */
                if (de->name_len == 1 && de->name[0] == '.') goto next;
                if (de->name_len == 2 && de->name[0] == '.' && de->name[1] == '.') goto next;
                if (de->file_type == JEXFS_DIRENT_DIR) terminal_putchar('/');
                for (uint16_t i = 0; i < de->name_len; i++)
                    terminal_putchar(de->name[i]);
                terminal_writestring("  ");
            }
next:
            uint16_t esz = 5 + de->name_len;
            bo += esz; off += esz;
        }
        /* Inner walk stopped mid-block or at its edge.  Advance to the
         * next block boundary so the outer loop always makes progress --
         * but only when not already on one: an entry that ends exactly at
         * the boundary would otherwise skip the whole next block, losing
         * every name stored after it. */
        if (off % BLOCK_SIZE)
            off += BLOCK_SIZE - (off % BLOCK_SIZE);
    }
    terminal_writestring("\n");
}

/**
 * jexfs_release_module -- Return the module root image to the PMM.
 *
 * The image is the GRUB-loaded multiboot module that pmm_init() pinned.
 * Once the disk is authoritative nothing reads it, so the frames become
 * usable heap memory.  Only the page-aligned interior is freed: the first
 * or last page can be shared with something the loader placed there.
 */
static void jexfs_release_module(void)
{
    if (!module_frame_count)
        return;
    pmm_free_blocks((void *)module_first_frame, module_frame_count);
    pr_info("Freed %u KB of module root image, %u KB now free\n",
            module_frame_count * (PMM_BLOCK_SIZE / 1024),
            pmm_get_free_memory() / 1024);
    module_first_frame = 0;
    module_frame_count = 0;
}

/**
 * jexfs_read_superblock -- Read and validate a JexFS superblock from disk.
 *
 * @param lba First sector of filesystem block 0
 * @param out Filled with the superblock on success
 * @return 0 on a valid superblock, -1 otherwise
 */
static int jexfs_read_superblock(uint32_t lba, struct jex_superblock *out)
{
    uint8_t buf[BLOCK_SIZE];

    if (ide_read_sector(lba + 2, buf) < 0 ||
        ide_read_sector(lba + 3, buf + 512) < 0)
        return -1;
    memcpy(out, buf, sizeof(*out));
    if (out->magic != JEXFS_MAGIC || out->inode_size != JEXFS_INODE_SIZE)
        return -1;
    return 0;
}

/**
 * jexfs_mount_disk -- Adopt the filesystem at lba as the mounted rootfs.
 *
 * Validates the superblock and checks that the described image actually
 * fits on the drive before committing anything, then switches block I/O
 * to the disk.
 *
 * @param lba First sector of filesystem block 0
 * @param expected_blocks Expected total_blocks, or 0 to accept any
 * @param total_sectors Drive capacity from IDENTIFY
 * @return 0 on success, -1 if there is no usable filesystem there
 */
static int jexfs_mount_disk(uint32_t lba, uint32_t expected_blocks,
                            uint32_t total_sectors)
{
    struct jex_superblock probe;

    if (jexfs_read_superblock(lba, &probe) < 0)
        return -1;
    if (probe.total_blocks == 0)
        return -1;
    if (expected_blocks && probe.total_blocks != expected_blocks)
        return -1;
    if ((uint64_t)lba + (uint64_t)probe.total_blocks * 2 > total_sectors)
        return -1;

    sb            = probe;
    disk_root_lba = lba;
    ramdisk_base  = NULL;
    cwd_inode     = 1;
    jexfs_mounted = 1;

    pr_info("Mounted from disk @ LBA %u: %u blocks, %u inodes\n",
            lba, sb.total_blocks, sb.total_inodes);
    return 0;
}

/**
 * jexfs_mount_persistence -- Choose the disk or the module as the rootfs.
 *
 * Prefers a JexFS on the primary master: either a raw image at LBA 0
 * (`make run`) or the copy a delivery image appends at the tail.  Mounting
 * the disk directly means the module is no longer the live rootfs, so its
 * pages are handed back to the PMM.  When no disk filesystem is found the
 * module stays live.
 *
 * Runs in process context (sleeping IDE mutex), after IRQs are enabled
 * and before anything touches JexFS blocks.
 */
void jexfs_mount_persistence(void)
{
    uint32_t total_sectors;

    /* The IDE driver has no initcall of its own; this is its only init
     * point, and it must not run from an initcall because reads sleep. */
    ide_init();

    if (ide_identify(&total_sectors) < 0) {
        pr_info("No disk found, rootfs stays %s\n",
                ramdisk_base ? "in the module" : "unmounted");
        return;
    }

    /* Raw JexFS at the start of the drive (`make run`). */
    if (jexfs_mount_disk(0, 0, total_sectors) == 0) {
        jexfs_release_module();
        return;
    }

    /* Delivery images append the rootfs after the ISO. */
    if (ramdisk_blocks && total_sectors > ramdisk_blocks * 2) {
        uint32_t tail = total_sectors - ramdisk_blocks * 2;
        if (jexfs_mount_disk(tail, ramdisk_blocks, total_sectors) == 0) {
            jexfs_release_module();
            return;
        }
    }

    if (ramdisk_base)
        pr_info("No disk rootfs, using module image @ 0x%08x\n",
                (uint32_t)ramdisk_base);
    else
        pr_info("No disk rootfs and no module image\n");
}

void jexfs_init(void)
{
    mutex_init(&fs_lock, "jexfs");
    pr_info("Mounting JexFS v2...\n");

    /* A GRUB-loaded multiboot module carries the root image in RAM.
     * Probe each module for a valid superblock; the disk is tried later,
     * from process context, because IDE reads sleep on a mutex. */
    if (g_mboot_info->flags & (1 << 3)) {
        multiboot_module_t *mod = (multiboot_module_t *)g_mboot_info->mods_addr;
        for (uint32_t i = 0; i < g_mboot_info->mods_count; i++, mod++) {
            uint32_t size = mod->end - mod->start;
            if (size < 2 * BLOCK_SIZE)
                continue;
            struct jex_superblock *probe =
                (struct jex_superblock *)(mod->start + BLOCK_SIZE);
            if (probe->magic == JEXFS_MAGIC &&
                probe->inode_size == JEXFS_INODE_SIZE) {
                jexfs_set_ramdisk((void *)mod->start, size);
                /* Remember the page-aligned interior for a later free. */
                uint32_t first = (mod->start + PMM_BLOCK_SIZE - 1)
                                 & ~(uint32_t)(PMM_BLOCK_SIZE - 1);
                uint32_t last  = mod->end & ~(uint32_t)(PMM_BLOCK_SIZE - 1);
                if (last > first) {
                    module_first_frame = first;
                    module_frame_count = (last - first) / PMM_BLOCK_SIZE;
                }
                pr_info("Root image from module @ 0x%08x, %u bytes\n",
                        mod->start, size);
                break;
            }
        }
    }

    /* Without a module the rootfs may live on the disk, but IDE reads
     * sleep and this runs from an initcall, so defer the mount. */
    if (!ramdisk_base) {
        pr_info("No module root image, disk mount deferred\n");
        return;
    }

    uint8_t buf[BLOCK_SIZE];
    read_block(1, buf);
    memcpy(&sb, buf, sizeof(sb));

    if (sb.magic != JEXFS_MAGIC) {
        pr_err("Invalid superblock magic (0x%08x)\n", sb.magic);
        return;
    }
    if (sb.inode_size != JEXFS_INODE_SIZE) {
        pr_err("Inode size mismatch (%u vs %u)\n", sb.inode_size, JEXFS_INODE_SIZE);
        return;
    }

    cwd_inode = 1;
    jexfs_mounted = 1;
    pr_info("Mounted: %u blocks, %u inodes, data at block %u\n",
            sb.total_blocks, sb.total_inodes, sb.data_start);
}

/* ── Locked public API ───────────────────────────────
 * Thin wrappers: take fs_lock, run the _nolock core, release.
 * The cores assume the caller holds fs_lock. */


void jexfs_read_inode(uint32_t idx, struct jex_inode *inode)
{
    mutex_lock(&fs_lock);
    jexfs_read_inode_raw(idx, inode);
    mutex_unlock(&fs_lock);
}


void jexfs_write_inode(uint32_t idx, struct jex_inode *inode)
{
    mutex_lock(&fs_lock);
    write_inode_nolock(idx, inode);
    mutex_unlock(&fs_lock);
}


void jexfs_stat(int inode_idx, struct jex_inode *inode)
{
    mutex_lock(&fs_lock);
    jexfs_stat_nolock(inode_idx, inode);
    mutex_unlock(&fs_lock);
}


int jexfs_open(const char *name)
{
    mutex_lock(&fs_lock);
    int ret = open_nolock(name);
    mutex_unlock(&fs_lock);
    return ret;
}


int jexfs_read(int inode_idx, void *buffer, uint32_t size, uint32_t offset)
{
    mutex_lock(&fs_lock);
    int ret = jexfs_read_nolock(inode_idx, buffer, size, offset);
    mutex_unlock(&fs_lock);
    return ret;
}


int jexfs_write(int inode_idx, const void *buffer, uint32_t size, uint32_t offset)
{
    mutex_lock(&fs_lock);
    int ret = jexfs_write_nolock(inode_idx, buffer, size, offset);
    mutex_unlock(&fs_lock);
    return ret;
}


int jexfs_create(const char *name)
{
    mutex_lock(&fs_lock);
    int ret = jexfs_create_nolock(name);
    mutex_unlock(&fs_lock);
    return ret;
}


int jexfs_mkdir(const char *name)
{
    mutex_lock(&fs_lock);
    int ret = jexfs_mkdir_nolock(name);
    mutex_unlock(&fs_lock);
    return ret;
}


int jexfs_remove(const char *name)
{
    mutex_lock(&fs_lock);
    int ret = jexfs_remove_nolock(name);
    mutex_unlock(&fs_lock);
    return ret;
}


int jexfs_rename(const char *old_name, const char *new_name)
{
    mutex_lock(&fs_lock);
    int ret = jexfs_rename_nolock(old_name, new_name);
    mutex_unlock(&fs_lock);
    return ret;
}


int jexfs_get_size(int inode_idx)
{
    mutex_lock(&fs_lock);
    int ret = jexfs_get_size_nolock(inode_idx);
    mutex_unlock(&fs_lock);
    return ret;
}


void jexfs_list_dir(uint32_t inode_idx)
{
    mutex_lock(&fs_lock);
    jexfs_list_dir_nolock(inode_idx);
    mutex_unlock(&fs_lock);
}
