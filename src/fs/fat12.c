/**
 * @file fat12.c
 * @brief FAT12 Filesystem Driver (RAM Disk).
 *
 * Implements a simple FAT12 driver that operates on an in-memory 1.44MB disk.
 * Used for temporary storage or boot compatibility experiments.
 */

#include "fat12.h"
#include "kheap.h"
#include <jexos/errno.h>
#include "terminal.h"
#include "init.h"
#include <stddef.h>

/**
 * @brief The 1.44MB RAM disk buffer.
 */
uint8_t* ram_disk;
#define RAM_DISK_SIZE (1440 * 1024)

/**
 * @brief Format a string into the standard 8.3 FAT12 directory entry format.
 * 
 * @param input The input filename (e.g., "test.txt").
 * @param output The 11-byte output buffer.
 */
void format_filename(const char* input, uint8_t* output) {
    int i = 0, j = 0;
    /* Fill with spaces */
    for(int k = 0; k < 11; k++) output[k] = ' ';

    /* Copy name part (up to 8 chars) */
    while (input[i] != '.' && input[i] != '\0' && j < 8) {
        output[j++] = input[i++];
    }

    /* Skip the dot separator */
    if (input[i] == '.') i++;

    /* Copy extension part (up to 3 chars) */
    j = 8;
    while (input[i] != '\0' && j < 11) {
        output[j++] = input[i++];
    }
}

/**
 * @brief Compare a raw FAT12 filename with a C-string.
 */
int fat12_filename_equal(uint8_t* raw, const char* search) {
    uint8_t formatted[11];
    format_filename(search, formatted);
    for (int i = 0; i < 11; i++) {
        if (raw[i] != formatted[i]) return 0;
    }
    return 1;
}

/**
 * @brief Initialize the FAT12 RAM disk.
 * Allocates memory and sets up a basic FAT12 boot sector.
 */
void init_fat12() {
    ram_disk = (uint8_t*)kmalloc(RAM_DISK_SIZE);
    memset(ram_disk, 0, RAM_DISK_SIZE);
    
    fat12_boot_sector_t* boot = (fat12_boot_sector_t*)ram_disk;
    boot->bytes_per_sector = 512;
    boot->sectors_per_cluster = 1;
    boot->reserved_sector_count = 1;
    boot->num_fats = 2;
    boot->root_entry_count = 224;
    boot->total_sectors_16 = 2880;
    boot->sectors_per_fat_16 = 9;
    boot->boot_sector_signature = 0xAA55;
    
    terminal_writestring("FAT12 RAM Disk Initialized (1.44MB).\n");
}

/**
 * @brief List files in the root directory.
 */
void fat12_ls() {
    uint32_t root_offset = 19 * 512; /* Root dir starts at sector 19 */
    fat12_entry_t* entries = (fat12_entry_t*)(ram_disk + root_offset);
    
    int found = 0;
    for (int i = 0; i < 224; i++) {
        if (entries[i].filename[0] == 0x00) break;   /* End of dir */
        if (entries[i].filename[0] == 0xE5) continue; /* Deleted file */
        
        found = 1;
        /* Print filename */
        for (int j = 0; j < 8; j++) {
            if (entries[i].filename[j] != ' ') terminal_putchar(entries[i].filename[j]);
        }
        /* Print extension */
        if (entries[i].extension[0] != ' ') {
            terminal_putchar('.');
            for (int j = 0; j < 3; j++) {
                if (entries[i].extension[j] != ' ') terminal_putchar(entries[i].extension[j]);
            }
        }
        terminal_writestring("  ");
    }
    if (found) terminal_writestring("\n");
    else terminal_writestring("(No files found)\n");
}

/**
 * @brief Create an empty file in the root directory.
 */
void fat12_touch(const char* name) {
    uint32_t root_offset = 19 * 512;
    fat12_entry_t* entries = (fat12_entry_t*)(ram_disk + root_offset);
    
    for (int i = 0; i < 224; i++) {
        if (entries[i].filename[0] == 0x00 || entries[i].filename[0] == 0xE5) {
            uint8_t formatted[11];
            format_filename(name, formatted);
            memcpy(entries[i].filename, formatted, 11);
            entries[i].file_size = 0;
            entries[i].attributes = 0x20; /* Archive bit set */
            if (entries[i].filename[0] == 0x00 && i < 223) entries[i+1].filename[0] = 0x00;
            return;
        }
    }
}

/**
 * @brief Simple write text to a file (one sector limit for simplicity).
 */
void fat12_echo(const char* name, const char* text) {
    uint32_t root_offset = 19 * 512;
    fat12_entry_t* entries = (fat12_entry_t*)(ram_disk + root_offset);
    
    for (int i = 0; i < 224; i++) {
        if (fat12_filename_equal(entries[i].filename, name)) {
            /* Map file data to a fixed sector for this simple driver */
            uint32_t data_offset = (33 + i) * 512;
            int len = 0;
            while (text[len] != '\0' && len < 511) {
                ram_disk[data_offset + len] = text[len];
                len++;
            }
            ram_disk[data_offset + len] = '\0';
            entries[i].file_size = len;
            return;
        }
    }
    terminal_writestring("File not found.\n");
}

/**
 * @brief Print file content to the terminal.
 */
void fat12_cat(const char* name) {
    uint32_t root_offset = 19 * 512;
    fat12_entry_t* entries = (fat12_entry_t*)(ram_disk + root_offset);
    
    for (int i = 0; i < 224; i++) {
        if (fat12_filename_equal(entries[i].filename, name)) {
            uint32_t data_offset = (33 + i) * 512;
            terminal_writestring((char*)(ram_disk + data_offset));
            terminal_writestring("\n");
            return;
        }
    }
    terminal_writestring("File not found.\n");
}

/**
 * @brief Mark a file as deleted.
 */
void fat12_rm(const char* name) {
    uint32_t root_offset = 19 * 512;
    fat12_entry_t* entries = (fat12_entry_t*)(ram_disk + root_offset);
    
    for (int i = 0; i < 224; i++) {
        if (fat12_filename_equal(entries[i].filename, name)) {
            entries[i].filename[0] = 0xE5; /* Standard FAT delete marker */
            return;
        }
    }
}

/**
 * @brief Read an entire file into a buffer.
 */
int fat12_read_file(const char* name, uint8_t* buffer) {
    uint32_t root_offset = 19 * 512;
    fat12_entry_t* entries = (fat12_entry_t*)(ram_disk + root_offset);
    for (int i = 0; i < 224; i++) {
        if (fat12_filename_equal(entries[i].filename, name)) {
            uint32_t data_offset = (33 + i) * 512;
            memcpy(buffer, ram_disk + data_offset, entries[i].file_size);
            return (int)entries[i].file_size;
        }
    }
    return -ENOENT;
}

/**
 * @brief Write raw data to a file.
 */
device_init(init_fat12);

void fat12_write_raw(const char* name, uint8_t* data, uint32_t size) {
    uint32_t root_offset = 19 * 512;
    fat12_entry_t* entries = (fat12_entry_t*)(ram_disk + root_offset);
    for (int i = 0; i < 224; i++) {
        if (fat12_filename_equal(entries[i].filename, name)) {
            uint32_t data_offset = (33 + i) * 512;
            memcpy(ram_disk + data_offset, data, size);
            entries[i].file_size = size;
            return;
        }
    }
}
