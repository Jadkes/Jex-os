#include "fat12.h"
#include "kheap.h"
#include <stddef.h>

extern void terminal_writestring(const char* data);
extern void terminal_putchar(char c);

/* 1.44 MB RAM Disk */
uint8_t* ram_disk;
#define RAM_DISK_SIZE (1440 * 1024)

/* Helper to compare filenames */
int fat12_filename_equal(const char* raw, const char* search) {
    // This is a simple version, FAT uses 8.3 format
    // For simplicity, we'll just check the first few chars
    for (int i = 0; i < 8; i++) {
        if (search[i] == '\0') return 1;
        if (raw[i] != search[i]) return 0;
    }
    return 1;
}

void init_fat12() {
    ram_disk = (uint8_t*)kmalloc(RAM_DISK_SIZE);
    
    /* Format the RAM disk with a basic FAT12 header */
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

void fat12_ls() {
    /* Root directory starts after Boot Sector + 2 FATs */
    /* 1 + (2 * 9) = 19 sectors */
    uint32_t root_offset = 19 * 512;
    fat12_entry_t* entries = (fat12_entry_t*)(ram_disk + root_offset);
    
    int found = 0;
    for (int i = 0; i < 224; i++) {
        if (entries[i].filename[0] == 0x00) break; // End of entries
        if (entries[i].filename[0] == 0xE5) continue; // Deleted
        
        found = 1;
        for (int j = 0; j < 8; j++) {
            if (entries[i].filename[j] == ' ') break;
            terminal_putchar(entries[i].filename[j]);
        }
        terminal_writestring("  ");
    }
    if (found) terminal_writestring("\n");
    else terminal_writestring("(No files found)\n");
}

void fat12_touch(const char* name) {
    uint32_t root_offset = 19 * 512;
    fat12_entry_t* entries = (fat12_entry_t*)(ram_disk + root_offset);
    
    for (int i = 0; i < 224; i++) {
        if (entries[i].filename[0] == 0x00 || entries[i].filename[0] == 0xE5) {
            // Found a spot
            for (int j = 0; j < 8; j++) {
                if (name[j] == '\0') entries[i].filename[j] = ' ';
                else entries[i].filename[j] = name[j];
            }
            entries[i].file_size = 0;
            entries[i].attributes = 0x20; // Archive
            if (entries[i].filename[0] == 0x00 && i < 223) {
                entries[i+1].filename[0] = 0x00;
            }
            return;
        }
    }
}

void fat12_echo(const char* name, const char* text) {
    uint32_t root_offset = 19 * 512;
    fat12_entry_t* entries = (fat12_entry_t*)(ram_disk + root_offset);
    
    for (int i = 0; i < 224; i++) {
        if (fat12_filename_equal((char*)entries[i].filename, name)) {
            /* Data starts after root dir (sector 19 + (224 * 32 / 512) = 19 + 14 = 33) */
            /* For this simple demo, we'll just give each file one sector starting at index i */
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

void fat12_cat(const char* name) {
    uint32_t root_offset = 19 * 512;
    fat12_entry_t* entries = (fat12_entry_t*)(ram_disk + root_offset);
    
    for (int i = 0; i < 224; i++) {
        if (fat12_filename_equal((char*)entries[i].filename, name)) {
            uint32_t data_offset = (33 + i) * 512;
            terminal_writestring((char*)(ram_disk + data_offset));
            terminal_writestring("\n");
            return;
        }
    }
    terminal_writestring("File not found.\n");
}

void fat12_rm(const char* name) {
    uint32_t root_offset = 19 * 512;
    fat12_entry_t* entries = (fat12_entry_t*)(ram_disk + root_offset);
    
    for (int i = 0; i < 224; i++) {
        if (fat12_filename_equal((char*)entries[i].filename, name)) {
            entries[i].filename[0] = 0xE5; // Mark as deleted
            return;
        }
    }
}
