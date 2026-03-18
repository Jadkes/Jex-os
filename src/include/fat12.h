/**
 * @file fat12.h
 * @brief FAT12 Filesystem Driver.
 *
 * Implements basic FAT12 support for reading and writing files on floppy/images.
 */

#ifndef FAT12_H
#define FAT12_H

#include <stdint.h>

/**
 * @struct fat12_boot_sector_t
 * @brief FAT12 BIOS Parameter Block (BPB) structure.
 */
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

/**
 * @struct fat12_entry_t
 * @brief FAT12 Directory Entry structure.
 */
typedef struct __attribute__((packed)) {
    uint8_t  filename[8];
    uint8_t  extension[3];
    uint8_t  attributes;
    uint8_t  reserved;
    uint8_t  creation_time_ms;
    uint16_t creation_time;
    uint16_t creation_date;
    uint16_t last_access_date;
    uint16_t first_cluster_hi; // For FAT32 compatibility
    uint16_t last_write_time;
    uint16_t last_write_date;
    uint16_t first_cluster_lo;
    uint32_t file_size;
} fat12_entry_t;

/**
 * @brief Initialize the FAT12 driver and mount the filesystem.
 */
void init_fat12();

/**
 * @brief List files in the root directory.
 */
void fat12_ls();

/**
 * @brief Create an empty file.
 * @param name Filename.
 */
void fat12_touch(const char* name);

/**
 * @brief Write text to a file (overwrite).
 * @param name Filename.
 * @param text Content to write.
 */
void fat12_echo(const char* name, const char* text);

/**
 * @brief Print file content to terminal.
 * @param name Filename.
 */
void fat12_cat(const char* name);

/**
 * @brief Delete a file.
 * @param name Filename.
 */
void fat12_rm(const char* name);

/**
 * @brief Read an entire file into a buffer.
 * @param name Filename.
 * @param buffer Pointer to the destination buffer.
 * @return 0 on success, -1 on failure.
 */
int fat12_read_file(const char* name, uint8_t* buffer);

/**
 * @brief Write raw data to a file.
 * @param name Filename.
 * @param data Source data.
 * @param size Data size in bytes.
 */
void fat12_write_raw(const char* name, uint8_t* data, uint32_t size);

#endif // FAT12_H
