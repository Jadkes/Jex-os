#include "fs.h"
#include "jexfs.h"
#include "kheap.h"
#include <stddef.h>

extern void terminal_writestring(const char* data);

file_descriptor_t file_table[MAX_OPEN_FILES];

void fs_init() {
    for (int i = 0; i < MAX_OPEN_FILES; i++) {
        file_table[i].used = 0;
    }
    jexfs_init();
}

int fs_open(const char* filename, int flags) {
    int fd = -1;
    for (int i = 0; i < MAX_OPEN_FILES; i++) {
        if (!file_table[i].used) {
            fd = i;
            break;
        }
    }
    if (fd == -1) return -1;

    int inode_idx = jexfs_open(filename);
    if (inode_idx == -1) {
        // If it doesn't exist, we could create it if flags say so.
        // For simplicity, let's just return error here.
        return -1;
    }

    file_table[fd].used = 1;
    file_table[fd].id = fd;
    file_table[fd].offset = 0;
    file_table[fd].dir_entry_idx = inode_idx; // Use this to store inode index

    return fd;
}

int fs_create(const char* filename) {
    return jexfs_create(filename);
}

int fs_read(int fd, void* buffer, uint32_t size) {
    if (fd < 0 || fd >= MAX_OPEN_FILES || !file_table[fd].used) return -1;
    int bytes = jexfs_read(file_table[fd].dir_entry_idx, buffer, size, file_table[fd].offset);
    if (bytes > 0) file_table[fd].offset += bytes;
    return bytes;
}

int fs_write(int fd, const void* buffer, uint32_t size) {
    if (fd < 0 || fd >= MAX_OPEN_FILES || !file_table[fd].used) return -1;
    int bytes = jexfs_write(file_table[fd].dir_entry_idx, buffer, size, file_table[fd].offset);
    if (bytes > 0) file_table[fd].offset += bytes;
    return bytes;
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
        int size = jexfs_get_size(file_table[fd].dir_entry_idx);
        file_table[fd].offset = size + offset;
    }
    return file_table[fd].offset;
}