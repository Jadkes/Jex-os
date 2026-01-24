#include "ide.h"
#include "ports.h"

extern void log_serial(const char* str);

static void ide_wait_busy() {
    while (inb(IDE_STATUS) & IDE_STATUS_BSY);
}

static void ide_wait_drq() {
    while (!(inb(IDE_STATUS) & IDE_STATUS_DRQ));
}

void ide_init() {
    log_serial("IDE: Initializing Primary Master...\n");
    // Select drive 0
    outb(IDE_DRIVE_SEL, 0xA0);
    ide_wait_busy();
}

int ide_read_sector(uint32_t lba, uint8_t* buffer) {
    ide_wait_busy();

    outb(IDE_DRIVE_SEL, 0xE0 | ((lba >> 24) & 0x0F));
    outb(IDE_SECTOR_CNT, 1);
    outb(IDE_LBA_LO, (uint8_t)lba);
    outb(IDE_LBA_MID, (uint8_t)(lba >> 8));
    outb(IDE_LBA_HI, (uint8_t)(lba >> 16));
    outb(IDE_COMMAND, IDE_CMD_READ);

    ide_wait_busy();
    
    if (inb(IDE_STATUS) & (IDE_STATUS_ERR | IDE_STATUS_DF)) {
        log_serial("IDE: Read error!\n");
        return -1;
    }

    ide_wait_drq();
    insw(IDE_DATA, buffer, 256); // 256 words = 512 bytes

    return 0;
}

int ide_write_sector(uint32_t lba, const uint8_t* buffer) {
    ide_wait_busy();

    outb(IDE_DRIVE_SEL, 0xE0 | ((lba >> 24) & 0x0F));
    outb(IDE_SECTOR_CNT, 1);
    outb(IDE_LBA_LO, (uint8_t)lba);
    outb(IDE_LBA_MID, (uint8_t)(lba >> 8));
    outb(IDE_LBA_HI, (uint8_t)(lba >> 16));
    outb(IDE_COMMAND, IDE_CMD_WRITE);

    ide_wait_busy();

    if (inb(IDE_STATUS) & (IDE_STATUS_ERR | IDE_STATUS_DF)) {
        log_serial("IDE: Write error!\n");
        return -1;
    }

    ide_wait_drq();
    outsw(IDE_DATA, buffer, 256);

    // Cache flush (optional for PIO but good practice)
    outb(IDE_COMMAND, 0xE7);
    ide_wait_busy();

    return 0;
}
