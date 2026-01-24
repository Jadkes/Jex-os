#include "jexfs.h"
#include "ide.h"
#include "kheap.h"
#include "string.h"

extern void log_serial(const char* str);

static struct jex_superblock sb;

void read_block(uint32_t block, uint8_t* buffer) {
    ide_read_sector(block * 2, buffer);
    ide_read_sector(block * 2 + 1, buffer + 512);
}

void write_block(uint32_t block, const uint8_t* buffer) {
    ide_write_sector(block * 2, buffer);
    ide_write_sector(block * 2 + 1, buffer + 512);
}

void jexfs_read_inode(uint32_t idx, struct jex_inode* inode) {
    uint8_t buf[BLOCK_SIZE];
    uint32_t block = sb.inode_table_start + (idx * sizeof(struct jex_inode)) / BLOCK_SIZE;
    uint32_t offset = (idx * sizeof(struct jex_inode)) % BLOCK_SIZE;
    read_block(block, buf);
    memcpy(inode, buf + offset, sizeof(struct jex_inode));
}

void jexfs_write_inode(uint32_t idx, struct jex_inode* inode) {
    uint8_t buf[BLOCK_SIZE];
    uint32_t block = sb.inode_table_start + (idx * sizeof(struct jex_inode)) / BLOCK_SIZE;
    uint32_t offset = (idx * sizeof(struct jex_inode)) % BLOCK_SIZE;
    read_block(block, buf);
    memcpy(buf + offset, inode, sizeof(struct jex_inode));
    write_block(block, buf);
}

void jexfs_init() {
    uint8_t buf[BLOCK_SIZE];
    read_block(1, buf);
    memcpy(&sb, buf, sizeof(struct jex_superblock));

    if (sb.magic != JEXFS_MAGIC) {
        log_serial("JexFS: Invalid magic! Filesystem corrupted.\n");
        return;
    }
    log_serial("JexFS: Initialized on /dev/hda\n");
}

int jexfs_open(const char* name) {
    uint8_t buf[BLOCK_SIZE];
    struct jex_inode root_inode;
    jexfs_read_inode(1, &root_inode);

    read_block(root_inode.blocks[0], buf);
    struct jex_dir_entry* de = (struct jex_dir_entry*)buf;

    for (int i = 0; i < DIR_ENTRIES_PER_BLOCK; i++) {
        if (de[i].inode != 0 && strcmp(de[i].name, name) == 0) {
            return de[i].inode;
        }
    }
    return -1;
}

int jexfs_read(int inode_idx, void* buffer, uint32_t size, uint32_t offset) {
    struct jex_inode inode;
    jexfs_read_inode(inode_idx, &inode);

    if (offset >= inode.size) return 0;
    if (offset + size > inode.size) size = inode.size - offset;

    uint32_t remaining = size;
    uint8_t* buf_ptr = (uint8_t*)buffer;
    uint32_t current_offset = offset;

    while (remaining > 0) {
        uint32_t block_idx = current_offset / BLOCK_SIZE;
        uint32_t block_offset = current_offset % BLOCK_SIZE;
        uint32_t read_now = BLOCK_SIZE - block_offset;
        if (read_now > remaining) read_now = remaining;

        if (block_idx >= 10 || inode.blocks[block_idx] == 0) break;

        uint8_t buf[BLOCK_SIZE];
        read_block(inode.blocks[block_idx], buf);
        memcpy(buf_ptr, buf + block_offset, read_now);

        remaining -= read_now;
        buf_ptr += read_now;
        current_offset += read_now;
    }

    return size - remaining;
}

uint32_t jexfs_alloc_block() {
    uint8_t bitmap[BLOCK_SIZE];
    read_block(sb.block_bitmap_start, bitmap);
    
    for (uint32_t i = 0; i < sb.total_blocks; i++) {
        if (!(bitmap[i / 8] & (1 << (i % 8)))) {
            bitmap[i / 8] |= (1 << (i % 8));
            write_block(sb.block_bitmap_start, bitmap);
            return i;
        }
    }
    return 0;
}

int jexfs_write(int inode_idx, const void* buffer, uint32_t size, uint32_t offset) {
    struct jex_inode inode;
    jexfs_read_inode(inode_idx, &inode);

    uint32_t remaining = size;
    const uint8_t* buf_ptr = (const uint8_t*)buffer;
    uint32_t current_offset = offset;

    while (remaining > 0) {
        uint32_t block_idx = current_offset / BLOCK_SIZE;
        uint32_t block_offset = current_offset % BLOCK_SIZE;
        uint32_t write_now = BLOCK_SIZE - block_offset;
        if (write_now > remaining) write_now = remaining;

        if (block_idx >= 10) break;

        if (inode.blocks[block_idx] == 0) {
            uint32_t new_block = jexfs_alloc_block();
            if (new_block == 0) return -1;
            inode.blocks[block_idx] = (uint16_t)new_block;
            jexfs_write_inode(inode_idx, &inode);
        }

        uint8_t buf[BLOCK_SIZE];
        read_block(inode.blocks[block_idx], buf);
        memcpy(buf + block_offset, buf_ptr, write_now);
        write_block(inode.blocks[block_idx], buf);

        remaining -= write_now;
        buf_ptr += write_now;
        current_offset += write_now;
    }

    if (offset + size > inode.size) {
        inode.size = offset + size;
        jexfs_write_inode(inode_idx, &inode);
    }

    return size - remaining;
}

int jexfs_create(const char* name) {
    // Check if it already exists
    int existing = jexfs_open(name);
    if (existing != -1) return existing;

    uint8_t bitmap[BLOCK_SIZE];
    read_block(sb.inode_bitmap_start, bitmap);
    int inode_idx = -1;
    for (int i = 0; i < (int)sb.total_inodes; i++) {
        if (!(bitmap[i / 8] & (1 << (i % 8)))) {
            bitmap[i / 8] |= (1 << (i % 8));
            inode_idx = i;
            break;
        }
    }
    if (inode_idx == -1) return -1;
    write_block(sb.inode_bitmap_start, bitmap);

    struct jex_inode inode;
    memset(&inode, 0, sizeof(inode));
    inode.mode = 1;
    inode.size = 0;
    jexfs_write_inode(inode_idx, &inode);

    struct jex_inode root_inode;
    jexfs_read_inode(1, &root_inode);
    uint8_t buf[BLOCK_SIZE];
    read_block(root_inode.blocks[0], buf);
    struct jex_dir_entry* de = (struct jex_dir_entry*)buf;
    for (int i = 0; i < DIR_ENTRIES_PER_BLOCK; i++) {
        if (de[i].inode == 0) {
            de[i].inode = (uint16_t)inode_idx;
            strncpy(de[i].name, name, 14);
            write_block(root_inode.blocks[0], buf);
            return inode_idx;
        }
    }
    return -1;
}

int jexfs_get_size(int inode_idx) {
    struct jex_inode inode;
    jexfs_read_inode(inode_idx, &inode);
    return inode.size;
}

void jexfs_list_dir() {
    struct jex_inode root_inode;
    jexfs_read_inode(1, &root_inode);
    uint8_t buf[BLOCK_SIZE];
    read_block(root_inode.blocks[0], buf);
    struct jex_dir_entry* de = (struct jex_dir_entry*)buf;
    
    for (int i = 0; i < DIR_ENTRIES_PER_BLOCK; i++) {
        if (de[i].inode != 0) {
            if (strcmp(de[i].name, ".") == 0 || strcmp(de[i].name, "..") == 0) continue;
            terminal_writestring(de[i].name);
            terminal_writestring("  ");
        }
    }
    terminal_writestring("\n");
}
