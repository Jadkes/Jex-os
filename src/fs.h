#ifndef FS_H
#define FS_H

#include <stdint.h>
#include "fat12.h"

#define MAX_OPEN_FILES 16

typedef struct {
    int id;                 
    int used;               
    uint32_t offset;        
    uint32_t file_size;     
    uint32_t data_start_sector; 
    fat12_entry_t dir_entry; 
    uint32_t dir_entry_idx;  
} file_descriptor_t;

void fs_init();
int fs_create(const char* filename);
int fs_open(const char* filename, int flags);
int fs_read(int fd, void* buffer, uint32_t size);
int fs_write(int fd, const void* buffer, uint32_t size);
void fs_close(int fd);
int fs_seek(int fd, int offset, int whence);

#endif