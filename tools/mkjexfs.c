/**
 * @file mkjexfs.c
 * @brief Host tool to create a JexFS disk image.
 *
 * This tool runs on the build host (e.g. Linux). It takes a directory 
 * (rootfs/) and packs its contents into a JexFS formatted image file.
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include "../src/include/jexfs.h"

#define IMG_SIZE (1440 * 1024)

/**
 * @brief Global image buffer.
 */
uint8_t* img;

/**
 * @brief Set a bit in the inode bitmap.
 */
void set_inode_bit(int i) { img[2 * BLOCK_SIZE + (i/8)] |= (1 << (i%8)); }

/**
 * @brief Set a bit in the data block bitmap.
 */
void set_block_bit(int i) { img[3 * BLOCK_SIZE + (i/8)] |= (1 << (i%8)); }

/**
 * @brief Main tool entry point.
 * 
 * Usage: ./mkjexfs <output.img>
 */
int main(int argc, char** argv) {
    if (argc < 2) {
        printf("Usage: %s <output.img>\n", argv[0]);
        return 1;
    }

    /* Allocate and clear the disk buffer */
    img = calloc(1, IMG_SIZE);
    
    /* Initialize the Superblock at Block 1 (Offset 1024) */
    struct jex_superblock* sb = (struct jex_superblock*)(img + BLOCK_SIZE);
    sb->magic = JEXFS_MAGIC;
    sb->total_blocks = 1440;
    sb->total_inodes = 128;
    sb->inode_bitmap_start = 2;
    sb->block_bitmap_start = 3;
    sb->inode_table_start = 4;
    sb->data_start = 12;

    /* Reserve blocks for system metadata */
    for(int i=0; i<12; i++) set_block_bit(i);
    
    /* Reserve Inode 0 (NULL) and Inode 1 (Root) */
    set_inode_bit(0); 
    set_inode_bit(1);

    /* Initialize Root Directory Inode (Inode 1) */
    struct jex_inode* root_inode = (struct jex_inode*)(img + 4 * BLOCK_SIZE + sizeof(struct jex_inode));
    root_inode->mode = 2; /* Directory */
    root_inode->size = BLOCK_SIZE;
    root_inode->blocks[0] = 12; /* First data block */
    set_block_bit(12);

    /* Setup initial directory entries (. and ..) */
    struct jex_dir_entry* root_dir = (struct jex_dir_entry*)(img + 12 * BLOCK_SIZE);
    root_dir[0].inode = 1; strcpy(root_dir[0].name, ".");
    root_dir[1].inode = 1; strcpy(root_dir[1].name, "..");
    
    int dir_count = 2;
    int next_inode = 2;
    int next_block = 13;

    /* Automatically pack files from the 'rootfs' directory */
    DIR* d = opendir("rootfs");
    if (d) {
        struct dirent* dir;
        while ((dir = readdir(d)) != NULL) {
            if (dir->d_name[0] == '.') continue;
            
            char path[256];
            sprintf(path, "rootfs/%s", dir->d_name);
            FILE* f = fopen(path, "rb");
            if (f) {
                fseek(f, 0, SEEK_END);
                uint32_t size = ftell(f);
                fseek(f, 0, SEEK_SET);

                /* Allocate and initialize a new Inode for the file */
                set_inode_bit(next_inode);
                struct jex_inode* ni = (struct jex_inode*)(img + 4 * BLOCK_SIZE + next_inode * sizeof(struct jex_inode));
                ni->mode = 1; /* File */
                ni->size = size;
                
                /* Copy file data into data blocks */
                int blocks_needed = (size + BLOCK_SIZE - 1) / BLOCK_SIZE;
                for(int b=0; b<blocks_needed && b<10; b++) {
                    ni->blocks[b] = next_block;
                    set_block_bit(next_block);
                    fread(img + next_block * BLOCK_SIZE, 1, BLOCK_SIZE, f);
                    next_block++;
                }

                /* Add directory entry to the root directory */
                root_dir[dir_count].inode = next_inode;
                strncpy(root_dir[dir_count].name, dir->d_name, 14);
                
                next_inode++;
                dir_count++;
                fclose(f);
                printf("Packed: %s (%u bytes)\n", dir->d_name, size);
            }
        }
        closedir(d);
    }

    /* Write the final image to the output file */
    FILE* out = fopen(argv[1], "wb");
    if (out) {
        fwrite(img, 1, IMG_SIZE, out);
        fclose(out);
    }

    free(img);
    return 0;
}
