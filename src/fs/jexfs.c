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
#include "kernel/printk.h"
#include "string.h"
#include "terminal.h"
#include <jexos/errno.h>

/* ── jfs_memcmp replacement (kernel has no libc) ── */
static inline int jfs_memcmp(const void *a, const void *b, uint32_t len) {
    const unsigned char *ca = (const unsigned char *)a;
    const unsigned char *cb = (const unsigned char *)b;
    for (uint32_t i = 0; i < len; i++)
        if (ca[i] != cb[i]) return (int)(ca[i]) - (int)(cb[i]);
    return 0;
}

/* ── Global state ── */
static struct jex_superblock sb;
uint32_t cwd_inode = 1;

/* ── Forward declarations ── */
static int jexfs_add_dirent(uint32_t dir_ino, uint16_t entry_ino,
                            const char *name, uint16_t name_len, uint8_t file_type);

/* ═════════════════════════════════════════════════════════════════════════════
 * Block I/O
 * ═════════════════════════════════════════════════════════════════════════════ */

void read_block(uint32_t block, uint8_t *buffer)
{
    ide_read_sector(block * 2, buffer);
    ide_read_sector(block * 2 + 1, buffer + 512);
}

void write_block(uint32_t block, const uint8_t *buffer)
{
    ide_write_sector(block * 2, buffer);
    ide_write_sector(block * 2 + 1, buffer + 512);
}

/* ═════════════════════════════════════════════════════════════════════════════
 * Bitmap allocator
 * ═════════════════════════════════════════════════════════════════════════════ */

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

/* ═════════════════════════════════════════════════════════════════════════════
 * Inode I/O
 * ═════════════════════════════════════════════════════════════════════════════ */

void jexfs_read_inode(uint32_t idx, struct jex_inode *inode)
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

void jexfs_write_inode(uint32_t idx, struct jex_inode *inode)
{
    if (idx >= sb.total_inodes) return;
    uint8_t buf[BLOCK_SIZE];
    uint32_t block = sb.inode_table_start + (idx * JEXFS_INODE_SIZE) / BLOCK_SIZE;
    uint32_t offset = (idx * JEXFS_INODE_SIZE) % BLOCK_SIZE;
    read_block(block, buf);
    memcpy(buf + offset, inode, JEXFS_INODE_SIZE);
    write_block(block, buf);
}

void jexfs_stat(int inode_idx, struct jex_inode *inode)
{
    jexfs_read_inode((uint32_t)inode_idx, inode);
}

/* ═════════════════════════════════════════════════════════════════════════════
 * Logical -> physical block mapping
 * ═════════════════════════════════════════════════════════════════════════════ */

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

/* ═════════════════════════════════════════════════════════════════════════════
 * Directory scanning
 * ═════════════════════════════════════════════════════════════════════════════ */

/**
 * jexfs_find_entry -- Look up a name in a directory inode.
 * @return Inode index on success, -ENOENT if not found.
 */
static int jexfs_find_entry(uint32_t dir_ino, const char *name, uint16_t name_len)
{
    struct jex_inode di;
    jexfs_read_inode(dir_ino, &di);
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
            if (de->inode == 0 && de->name_len == 0) return -ENOENT;
            if (de->inode != 0 && de->name_len == name_len
                && jfs_memcmp(de->name, name, name_len) == 0)
                return (int)de->inode;
            uint16_t esz = 5 + de->name_len;
            bo += esz; off += esz;
        }
    }
    return -ENOENT;
}

int jexfs_open(const char *name)
{
    if (!name || name[0] == '\0') return (int)cwd_inode;
    uint32_t dir = (name[0] == '/') ? 1 : cwd_inode;
    if (name[0] == '/') name++;
    if (name[0] == '\0') return 1;
    return jexfs_find_entry(dir, name, (uint16_t)strlen(name));
}

/* ═════════════════════════════════════════════════════════════════════════════
 * Read / Write
 * ═════════════════════════════════════════════════════════════════════════════ */

int jexfs_read(int inode_idx, void *buffer, uint32_t size, uint32_t offset)
{
    struct jex_inode inode;
    jexfs_read_inode((uint32_t)inode_idx, &inode);
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

int jexfs_write(int inode_idx, const void *buffer,
                uint32_t size, uint32_t offset)
{
    struct jex_inode inode;
    jexfs_read_inode((uint32_t)inode_idx, &inode);

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
                jexfs_write_inode((uint32_t)inode_idx, &inode);
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
    jexfs_write_inode((uint32_t)inode_idx, &inode);
    return (int)(size - remain);
}

/* ═════════════════════════════════════════════════════════════════════════════
 * File / Directory operations
 * ═════════════════════════════════════════════════════════════════════════════ */

int jexfs_create(const char *name)
{
    if (!name || name[0] == '\0') return -EINVAL;
    uint16_t nl = (uint16_t)strlen(name);
    if (nl > 255) return -ENAMETOOLONG;

    int existing = jexfs_open(name);
    if (existing >= 0) return existing;

    int ino = jexfs_alloc_inode();
    if (ino < 0) return ino;

    struct jex_inode inode;
    memset(&inode, 0, sizeof(inode));
    inode.mode = JEXFS_TYPE_FILE | JEXFS_IRUSR | JEXFS_IWUSR
                 | JEXFS_IRGRP | JEXFS_IROTH;
    jexfs_write_inode((uint32_t)ino, &inode);

    int ret = jexfs_add_dirent(cwd_inode, (uint16_t)ino, name, nl, JEXFS_DIRENT_FILE);
    if (ret < 0) { jexfs_free_inode((uint32_t)ino); return ret; }
    return ino;
}

int jexfs_mkdir(const char *name)
{
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
    jexfs_write_inode((uint32_t)ino, &inode);

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
    jexfs_read_inode(dir_ino, &di);
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
                found_off = (int)off; goto write_entry;
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
        jexfs_write_inode(dir_ino, &di);

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

        read_block(phys, buf);
        struct jex_dir_entry *de = (struct jex_dir_entry *)(buf + bo);
        de->inode = entry_ino;
        de->name_len = name_len;
        de->file_type = file_type;
        memcpy(de->name, name, name_len);

        uint32_t new_end = (uint32_t)found_off + (uint32_t)esz;
        if (new_end > di.size) {
            di.size = new_end;
            jexfs_write_inode(dir_ino, &di);
        }
        write_block(phys, buf);
    }
    return 0;
}

/* ── Remove ── */

int jexfs_remove(const char *name)
{
    if (!name || name[0] == '\0') return -EINVAL;
    uint16_t nl = (uint16_t)strlen(name);

    struct jex_inode di;
    jexfs_read_inode(cwd_inode, &di);
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
            if (de->inode == 0 && de->name_len == 0) goto not_found;
            if (de->inode != 0 && de->name_len == nl
                && jfs_memcmp(de->name, name, nl) == 0) {
                /* Found -- free inode and blocks */
                struct jex_inode inode;
                jexfs_read_inode(de->inode, &inode);

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
    }
not_found:
    return -ENOENT;
}

/* ── Rename ── */

int jexfs_rename(const char *old_name, const char *new_name)
{
    if (!old_name || !new_name) return -EINVAL;
    uint16_t old_nl = (uint16_t)strlen(old_name);
    uint16_t new_nl = (uint16_t)strlen(new_name);

    struct jex_inode di;
    jexfs_read_inode(cwd_inode, &di);
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
    }
    return -ENOENT;
}

int jexfs_get_size(int inode_idx)
{
    struct jex_inode inode;
    jexfs_read_inode((uint32_t)inode_idx, &inode);
    return (int)inode.size;
}

/* ═════════════════════════════════════════════════════════════════════════════
 * Directory listing (legacy -- new ls uses its own scanner)
 * ═════════════════════════════════════════════════════════════════════════════ */

void jexfs_list_dir(uint32_t inode_idx)
{
    struct jex_inode di;
    jexfs_read_inode(inode_idx, &di);
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
            if (de->inode == 0 && de->name_len == 0) goto list_end;
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
    }
list_end:
    terminal_writestring("\n");
}

/* ═════════════════════════════════════════════════════════════════════════════
 * Init
 * ═════════════════════════════════════════════════════════════════════════════ */

void jexfs_init(void)
{
    pr_info("Mounting JexFS v2...\n");
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
    pr_info("Mounted: %u blocks, %u inodes, data at block %u\n",
            sb.total_blocks, sb.total_inodes, sb.data_start);
}
