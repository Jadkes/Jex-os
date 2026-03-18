/**
 * @file ide.h
 * @brief IDE Disk Controller driver (PIO mode).
 *
 * Provides basic sector-level read/write access to the hard disk.
 */

#ifndef IDE_H
#define IDE_H

#include <stdint.h>

/* IDE Ports */
#define IDE_DATA        0x1F0
#define IDE_ERROR       0x1F1
#define IDE_SECTOR_CNT  0x1F2
#define IDE_LBA_LO      0x1F3
#define IDE_LBA_MID     0x1F4
#define IDE_LBA_HI      0x1F5
#define IDE_DRIVE_SEL   0x1F6
#define IDE_COMMAND     0x1F7
#define IDE_STATUS      0x1F7

/* IDE Commands */
#define IDE_CMD_READ    0x20
#define IDE_CMD_WRITE   0x30

/* IDE Status bits */
#define IDE_STATUS_BSY  0x80 /**< Busy. */
#define IDE_STATUS_DRDY 0x40 /**< Drive ready. */
#define IDE_STATUS_DF   0x20 /**< Drive fault. */
#define IDE_STATUS_DRQ  0x08 /**< Data request ready. */
#define IDE_STATUS_ERR  0x01 /**< Error. */

/**
 * @brief Initialize the IDE controller.
 */
void ide_init(void);

/**
 * @brief Read a single 512-byte sector using LBA addressing.
 * 
 * @param lba Logical Block Address.
 * @param buffer Pointer to the 512-byte destination buffer.
 * @return 0 on success, -1 on failure.
 */
int ide_read_sector(uint32_t lba, uint8_t* buffer);

/**
 * @brief Write a single 512-byte sector using LBA addressing.
 * 
 * @param lba Logical Block Address.
 * @param buffer Pointer to the 512-byte source data.
 * @return 0 on success, -1 on failure.
 */
int ide_write_sector(uint32_t lba, const uint8_t* buffer);

#endif // IDE_H
