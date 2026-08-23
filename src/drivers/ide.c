/**
 * @file ide.c
 * @brief IDE (PATA) disk driver using PIO mode.
 *
 * This driver provides simple sector-level access to the primary master hard disk
 * without using DMA or interrupts (Polling/PIO mode).
 */

#define pr_fmt(fmt) "[IDE] " fmt
#include "kernel/printk.h"
#include "ide.h"
#include "ports.h"
#include "serial.h"
#include "string.h"
#include <jexos/errno.h>

/* ── Sector cache ──────────────────────────────────────
 * JexFS reads are 1KB (two sectors) and file metadata sits in the same
 * handful of blocks, so a small LRU absorbs most of the PIO cost.
 * Write-through: writes update both media and any cached copy.  All
 * cache state is touched with interrupts disabled because a task can be
 * preempted between lookup and fill. */
#define IDE_CACHE_SECTORS 64

typedef struct {
    uint32_t lba;
    uint32_t stamp;      /* higher = more recently used; 0 = free line */
    uint8_t  data[512];
} ide_cache_line_t;

static ide_cache_line_t sector_cache[IDE_CACHE_SECTORS];
static uint32_t cache_clock;

/**
 * @brief Find the cached copy of a sector, refreshing its LRU stamp.
 *
 * @param lba Sector to look up
 * @return Cache line holding valid data for lba, or NULL on miss
 */
static ide_cache_line_t* cache_find(uint32_t lba) {
    for (int i = 0; i < IDE_CACHE_SECTORS; i++) {
        ide_cache_line_t* line = &sector_cache[i];
        if (line->stamp && line->lba == lba) {
            line->stamp = ++cache_clock;
            return line;
        }
    }
    return NULL;
}

/**
 * @brief Claim a cache line for a new sector, evicting the LRU entry.
 *
 * @param lba Sector that will be stored in the claimed line
 * @return Claimed line, stamped as most-recently-used but NOT yet filled
 */
static ide_cache_line_t* cache_claim(uint32_t lba) {
    /* Free lines first, then the oldest stamp */
    ide_cache_line_t* victim = NULL;
    for (int i = 0; i < IDE_CACHE_SECTORS && !victim; i++)
        if (!sector_cache[i].stamp)
            victim = &sector_cache[i];

    if (!victim) {
        victim = &sector_cache[0];
        for (int i = 1; i < IDE_CACHE_SECTORS; i++)
            if (sector_cache[i].stamp < victim->stamp)
                victim = &sector_cache[i];
    }

    victim->lba = lba;
    victim->stamp = ++cache_clock;
    return victim;
}

/**
 * @brief Polling loop to wait for the BSY bit to clear.
 *
 * Returns 0 on success, -1 on timeout (~1 second).
 */
static int ide_wait_busy() {
    int timeout = 10000000; /* ~1 second at typical port I/O speed */
    while (inb(IDE_STATUS) & IDE_STATUS_BSY) {
        if (--timeout <= 0) {
            pr_debug("ide_wait_busy: timeout\n");
            return -EIO;
        }
    }
    return 0;
}

/**
 * @brief Polling loop to wait for the DRQ (Data Request) bit to set.
 *
 * Returns 0 on success, -1 on timeout (~1 second).
 */
static int ide_wait_drq() {
    int timeout = 10000000;
    while (!(inb(IDE_STATUS) & IDE_STATUS_DRQ)) {
        if (--timeout <= 0) {
            pr_debug("ide_wait_drq: timeout\n");
            return -EIO;
        }
    }
    return 0;
}

/**
 * @brief Initialize the primary master IDE drive.
 */
void ide_init() {
    pr_info("Initializing Primary Master...\n");
    /* Select master drive on the primary bus */
    outb(IDE_DRIVE_SEL, 0xA0);
    ide_wait_busy();
}

/**
 * @brief Issue a PIO read of one sector straight from the drive.
 *
 * @param lba 28-bit Logical Block Address.
 * @param buffer Destination buffer (at least 512 bytes).
 * @return 0 on success, -EIO on error or timeout.
 */
static int ide_read_sector_raw(uint32_t lba, uint8_t* buffer) {
    ide_wait_busy();

    outb(IDE_DRIVE_SEL, 0xE0 | ((lba >> 24) & 0x0F));
    outb(IDE_SECTOR_CNT, 1);
    outb(IDE_LBA_LO, (uint8_t)lba);
    outb(IDE_LBA_MID, (uint8_t)(lba >> 8));
    outb(IDE_LBA_HI, (uint8_t)(lba >> 16));
    outb(IDE_COMMAND, IDE_CMD_READ);

    ide_wait_busy();
    if (inb(IDE_STATUS) & (IDE_STATUS_ERR | IDE_STATUS_DF)) {
        pr_err("Read error!\n");
        return -EIO;
    }
    ide_wait_drq();

    /* Read 256 words (512 bytes) from the data port */
    insw(IDE_DATA, buffer, 256);
    return 0;
}

/**
 * @brief Issue a PIO write of one sector straight to the drive.
 *
 * @param lba 28-bit Logical Block Address.
 * @param buffer Source buffer (at least 512 bytes).
 * @return 0 on success, -EIO on error or timeout.
 */
static int ide_write_sector_raw(uint32_t lba, const uint8_t* buffer) {
    ide_wait_busy();

    outb(IDE_DRIVE_SEL, 0xE0 | ((lba >> 24) & 0x0F));
    outb(IDE_SECTOR_CNT, 1);
    outb(IDE_LBA_LO, (uint8_t)lba);
    outb(IDE_LBA_MID, (uint8_t)(lba >> 8));
    outb(IDE_LBA_HI, (uint8_t)(lba >> 16));
    outb(IDE_COMMAND, IDE_CMD_WRITE);

    ide_wait_busy();
    if (inb(IDE_STATUS) & (IDE_STATUS_ERR | IDE_STATUS_DF)) {
        pr_err("Write error!\n");
        return -EIO;
    }
    ide_wait_drq();

    /* Send 256 words (512 bytes) to the data port */
    outsw(IDE_DATA, buffer, 256);

    /* Send CACHE FLUSH command to ensure data is written to media */
    outb(IDE_COMMAND, 0xE7);
    ide_wait_busy();
    return 0;
}

/**
 * @brief Read a 512-byte sector using LBA addressing (cache-backed).
 *
 * @param lba Logical Block Address.
 * @param buffer Destination buffer (must be at least 512 bytes).
 * @return 0 on success, -1 on failure.
 */
int ide_read_sector(uint32_t lba, uint8_t* buffer) {
    uint32_t flags = irq_save();

    ide_cache_line_t* line = cache_find(lba);
    if (!line) {
        line = cache_claim(lba);
        if (ide_read_sector_raw(lba, line->data) != 0) {
            /* Don't leave a poisoned line in the cache */
            line->stamp = 0;
            irq_restore(flags);
            return -1;
        }
    }
    memcpy(buffer, line->data, 512);

    irq_restore(flags);
    return 0;
}

/**
 * @brief Write a 512-byte sector using LBA addressing (write-through).
 *
 * @param lba Logical Block Address.
 * @param buffer Source data buffer (must be 512 bytes).
 * @return 0 on success, -1 on failure.
 */
int ide_write_sector(uint32_t lba, const uint8_t* buffer) {
    uint32_t flags = irq_save();

    if (ide_write_sector_raw(lba, buffer) != 0) {
        irq_restore(flags);
        return -1;
    }

    ide_cache_line_t* line = cache_find(lba);
    if (!line)
        line = cache_claim(lba);
    memcpy(line->data, buffer, 512);

    irq_restore(flags);
    return 0;
}
