#include "fat12.h"
#include "kheap.h"
#include <stddef.h>

extern void terminal_writestring(const char* data);
extern void terminal_putchar(char c);

uint8_t* ram_disk;
#define RAM_DISK_SIZE (1440 * 1024)

/* 
   Helper to format a string into a FAT12 11-byte filename 
   Preserves case for JexOS aesthetics.
*/
void format_filename(const char* input, uint8_t* output) {
    int i = 0, j = 0;
    // Fill with spaces
    for(int k = 0; k < 11; k++) output[k] = ' ';

    // Copy name
    while (input[i] != '.' && input[i] != '\0' && j < 8) {
        output[j++] = input[i++];
    }

    // Skip the dot
    if (input[i] == '.') i++;

    // Copy extension
    j = 8;
    while (input[i] != '\0' && j < 11) {
        output[j++] = input[i++];
    }
}

int fat12_filename_equal(uint8_t* raw, const char* search) {
    uint8_t formatted[11];
    format_filename(search, formatted);
    for (int i = 0; i < 11; i++) {
        if (raw[i] != formatted[i]) return 0;
    }
    return 1;
}

void init_fat12() {
    ram_disk = (uint8_t*)kmalloc(RAM_DISK_SIZE);
    /* Zero out to ensure clean directory */
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

void fat12_ls() {
    uint32_t root_offset = 19 * 512;
    fat12_entry_t* entries = (fat12_entry_t*)(ram_disk + root_offset);
    
    int found = 0;
    for (int i = 0; i < 224; i++) {
        if (entries[i].filename[0] == 0x00) break;
        if (entries[i].filename[0] == 0xE5) continue;
        
        found = 1;
        // Print Name
        for (int j = 0; j < 8; j++) {
            if (entries[i].filename[j] != ' ') terminal_putchar(entries[i].filename[j]);
        }
        // Print Extension if exists
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

void fat12_touch(const char* name) {
    uint32_t root_offset = 19 * 512;
    fat12_entry_t* entries = (fat12_entry_t*)(ram_disk + root_offset);
    
    for (int i = 0; i < 224; i++) {
        if (entries[i].filename[0] == 0x00 || entries[i].filename[0] == 0xE5) {
            uint8_t formatted[11];
            format_filename(name, formatted);
            memcpy(entries[i].filename, formatted, 11); // Copy all 11 bytes
            entries[i].file_size = 0;
            entries[i].attributes = 0x20;
            if (entries[i].filename[0] == 0x00 && i < 223) entries[i+1].filename[0] = 0x00;
            return;
        }
    }
}

void fat12_echo(const char* name, const char* text) {
    uint32_t root_offset = 19 * 512;
    fat12_entry_t* entries = (fat12_entry_t*)(ram_disk + root_offset);
    
    for (int i = 0; i < 224; i++) {
        if (fat12_filename_equal(entries[i].filename, name)) {
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
        if (fat12_filename_equal(entries[i].filename, name)) {
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
        if (fat12_filename_equal(entries[i].filename, name)) {
            entries[i].filename[0] = 0xE5;
            return;
        }
    }
}

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
    return -1;
}

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
