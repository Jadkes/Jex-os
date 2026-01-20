#include "fs.h"
#include "fat12.h"
#include "kheap.h"
#include <stddef.h>

extern uint8_t* ram_disk; // From fat12.c
extern int fat12_filename_equal(const char* raw, const char* search); // From fat12.c
extern void terminal_writestring(const char* data);

file_descriptor_t file_table[MAX_OPEN_FILES];

void fs_init() {
    for (int i = 0; i < MAX_OPEN_FILES; i++) {
        file_table[i].used = 0;
    }
}

int fs_open(const char* filename, int flags) {
    // 1. Find a free FD slot
    int fd = -1;
    for (int i = 0; i < MAX_OPEN_FILES; i++) {
        if (!file_table[i].used) {
            fd = i;
            break;
        }
    }
    if (fd == -1) return -1; // Too many open files

    // 2. Find file in FAT12
    uint32_t root_offset = 19 * 512;
    fat12_entry_t* entries = (fat12_entry_t*)(ram_disk + root_offset);
    
    int found_idx = -1;
    for (int i = 0; i < 224; i++) {
        if (entries[i].filename[0] == 0x00) break;
        if (entries[i].filename[0] == 0xE5) continue;
        
        // Convert FAT12 name to string for comparison or reuse helper
        // We'll trust the helper can handle it if we modify it or 
        // reimplement simple check here.
        // Let's implement a direct check here to be safe and dependent only on RAM
        
        // ... Assuming fat12_filename_equal works on raw entries
        if (fat12_filename_equal((char*)entries[i].filename, filename)) {
            found_idx = i;
            break;
        }
    }

    if (found_idx == -1) {
        // File not found. If flags includes create, we could make it.
        // For now, let's assume it must exist (use 'touch' first).
        return -1;
    }

    // 3. Populate FD
    file_table[fd].used = 1;
    file_table[fd].id = fd;
    file_table[fd].offset = 0;
    file_table[fd].file_size = entries[found_idx].file_size;
    file_table[fd].dir_entry = entries[found_idx];
    file_table[fd].dir_entry_idx = found_idx;
    
    // Data starts at sector 33 (19 + 14)
    file_table[fd].data_start_sector = (33 + found_idx) * 512; 
    // ^ This is our simplified allocator (1 file = 1 sector/cluster fixed at index)

    return fd;
}

int fs_read(int fd, void* buffer, uint32_t size) {
    if (fd < 0 || fd >= MAX_OPEN_FILES || !file_table[fd].used) return -1;

    if (file_table[fd].offset >= file_table[fd].file_size) return 0; // EOF

    if (file_table[fd].offset + size > file_table[fd].file_size) {
        size = file_table[fd].file_size - file_table[fd].offset;
    }

    uint8_t* disk_ptr = ram_disk + file_table[fd].data_start_sector + file_table[fd].offset;
    memcpy(buffer, disk_ptr, size);
    
    file_table[fd].offset += size;
    return size;
}

int fs_write(int fd, const void* buffer, uint32_t size) {
    if (fd < 0 || fd >= MAX_OPEN_FILES || !file_table[fd].used) return -1;

    // Simple check: don't write past 512 bytes (our 1-cluster limit for now)
    if (file_table[fd].offset + size > 512) {
        size = 512 - file_table[fd].offset;
    }

    uint8_t* disk_ptr = ram_disk + file_table[fd].data_start_sector + file_table[fd].offset;
    memcpy(disk_ptr, buffer, size);
    
    file_table[fd].offset += size;
    
    // Update file size if we extended it
    if (file_table[fd].offset > file_table[fd].file_size) {
        file_table[fd].file_size = file_table[fd].offset;
        
        // Update directory entry on disk
        uint32_t root_offset = 19 * 512;
        fat12_entry_t* entries = (fat12_entry_t*)(ram_disk + root_offset);
        entries[file_table[fd].dir_entry_idx].file_size = file_table[fd].file_size;
    }

    return size;
}

void fs_close(int fd) {
    if (fd >= 0 && fd < MAX_OPEN_FILES) {
        file_table[fd].used = 0;
    }
}

int fs_seek(int fd, int offset, int whence) {
    if (fd < 0 || fd >= MAX_OPEN_FILES || !file_table[fd].used) return -1;
    
    if (whence == 0) { // SEEK_SET
        file_table[fd].offset = offset;
    } else if (whence == 1) { // SEEK_CUR
        file_table[fd].offset += offset;
    } else if (whence == 2) { // SEEK_END
        file_table[fd].offset = file_table[fd].file_size + offset;
    }
    return file_table[fd].offset;
}
