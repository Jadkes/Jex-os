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
            return -1;
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
            return -1;
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
 * @brief Read a 512-byte sector using LBA28 addressing.
 * 
 * @param lba 28-bit Logical Block Address.
 * @param buffer Destination buffer (must be at least 512 bytes).
 * @return 0 on success, -1 on error.
 */
int ide_read_sector(uint32_t lba, uint8_t* buffer) {
    ide_wait_busy();

    /* Send LBA and sector count to IDE controller */
    outb(IDE_DRIVE_SEL, 0xE0 | ((lba >> 24) & 0x0F));
    outb(IDE_SECTOR_CNT, 1);
    outb(IDE_LBA_LO, (uint8_t)lba);
    outb(IDE_LBA_MID, (uint8_t)(lba >> 8));
    outb(IDE_LBA_HI, (uint8_t)(lba >> 16));
    outb(IDE_COMMAND, IDE_CMD_READ);

    ide_wait_busy();
    
    /* Check for errors in the status register */
    if (inb(IDE_STATUS) & (IDE_STATUS_ERR | IDE_STATUS_DF)) {
        pr_err("Read error!\n");
        return -1;
    }

    /* Wait for drive to have data ready */
    ide_wait_drq();
    
    /* Read 256 words (512 bytes) from the data port */
    insw(IDE_DATA, buffer, 256);

    return 0;
}

/**
 * @brief Write a 512-byte sector using LBA28 addressing.
 * 
 * @param lba 28-bit Logical Block Address.
 * @param buffer Source data buffer (must be 512 bytes).
 * @return 0 on success, -1 on error.
 */
int ide_write_sector(uint32_t lba, const uint8_t* buffer) {
    ide_wait_busy();

    /* Send LBA and sector count */
    outb(IDE_DRIVE_SEL, 0xE0 | ((lba >> 24) & 0x0F));
    outb(IDE_SECTOR_CNT, 1);
    outb(IDE_LBA_LO, (uint8_t)lba);
    outb(IDE_LBA_MID, (uint8_t)(lba >> 8));
    outb(IDE_LBA_HI, (uint8_t)(lba >> 16));
    outb(IDE_COMMAND, IDE_CMD_WRITE);

    ide_wait_busy();

    /* Check for errors */
    if (inb(IDE_STATUS) & (IDE_STATUS_ERR | IDE_STATUS_DF)) {
        pr_err("Write error!\n");
        return -1;
    }

    /* Wait for drive to be ready to receive data */
    ide_wait_drq();
    
    /* Send 256 words (512 bytes) to the data port */
    outsw(IDE_DATA, buffer, 256);

    /* Send CACHE FLUSH command to ensure data is written to media */
    outb(IDE_COMMAND, 0xE7);
    ide_wait_busy();

    return 0;
}
