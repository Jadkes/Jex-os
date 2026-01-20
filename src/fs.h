#ifndef FS_H
#define FS_H

#include <stdint.h>
#include "fat12.h"

#define MAX_OPEN_FILES 16

typedef struct {
    int id;                 // File Descriptor ID (0-15)
    int used;               // Is this slot used?
    uint32_t offset;        // Current Read/Write pointer
    uint32_t file_size;     // Size of the file
    uint32_t data_start_sector; // Where data begins on disk
    fat12_entry_t dir_entry; // Cached directory entry
    uint32_t dir_entry_idx;  // Index in root directory (to update size later)
} file_descriptor_t;

void fs_init();
int fs_open(const char* filename, int flags);
int fs_read(int fd, void* buffer, uint32_t size);
int fs_write(int fd, const void* buffer, uint32_t size);
void fs_close(int fd);
int fs_seek(int fd, int offset, int whence);

#endif
