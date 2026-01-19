#ifndef FAT12_H
#define FAT12_H

#include <stdint.h>

typedef struct __attribute__((packed)) {
    uint8_t  jmp_boot[3];
    uint8_t  oem_name[8];
    uint16_t bytes_per_sector;
    uint8_t  sectors_per_cluster;
    uint16_t reserved_sector_count;
    uint8_t  num_fats;
    uint16_t root_entry_count;
    uint16_t total_sectors_16;
    uint8_t  media_descriptor;
    uint16_t sectors_per_fat_16;
    uint16_t sectors_per_track;
    uint16_t num_heads;
    uint32_t hidden_sectors;
    uint32_t total_sectors_32;
    uint8_t  drive_number;
    uint8_t  reserved_1;
    uint8_t  boot_signature;
    uint32_t volume_id;
    uint8_t  volume_label[11];
    uint8_t  filesystem_type[8];
    uint8_t  boot_code[448];
    uint16_t boot_sector_signature;
} fat12_boot_sector_t;

typedef struct __attribute__((packed)) {
    uint8_t  filename[8];
    uint8_t  extension[3];
    uint8_t  attributes;
    uint8_t  reserved;
    uint8_t  creation_time_ms;
    uint16_t creation_time;
    uint16_t creation_date;
    uint16_t last_access_date;
    uint16_t first_cluster_hi; // FAT32
    uint16_t last_write_time;
    uint16_t last_write_date;
    uint16_t first_cluster_lo;
    uint32_t file_size;
} fat12_entry_t;

void init_fat12();
void fat12_ls();
void fat12_touch(const char* name);
void fat12_echo(const char* name, const char* text);
void fat12_cat(const char* name);
void fat12_rm(const char* name);

#endif
