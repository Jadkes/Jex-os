#include "pmm.h"
#include <stddef.h>

/* Helper for memset */
void* memset(void* bufptr, int value, size_t size) {
	unsigned char* buf = (unsigned char*) bufptr;
	for (size_t i = 0; i < size; i++)
		buf[i] = (unsigned char) value;
	return bufptr;
}

/* 
   We will manage up to 128MB of RAM for now to keep the bitmap small.
   128MB / 4KB blocks = 32,768 blocks.
   32,768 bits / 8 bits per byte = 4096 bytes (4KB) for the bitmap.
*/
#define RAM_SIZE_MB 128
#define BLOCKS_PER_BYTE 8
#define BLOCK_SIZE 4096
#define TOTAL_BLOCKS (RAM_SIZE_MB * 1024 * 1024 / BLOCK_SIZE)
#define BITMAP_SIZE (TOTAL_BLOCKS / BLOCKS_PER_BYTE)

uint8_t pmm_bitmap[BITMAP_SIZE];
uint32_t used_blocks = 0;
uint32_t max_blocks = TOTAL_BLOCKS;

/* Set a bit in the bitmap (mark as used) */
void mmap_set(int bit) {
    pmm_bitmap[bit / 8] |= (1 << (bit % 8));
}

/* Unset a bit in the bitmap (mark as free) */
void mmap_unset(int bit) {
    pmm_bitmap[bit / 8] &= ~(1 << (bit % 8));
}

/* Check if a bit is set */
int mmap_test(int bit) {
    return pmm_bitmap[bit / 8] & (1 << (bit % 8));
}

/* Find the first free block (bit 0) */
int mmap_first_free() {
    for (uint32_t i = 0; i < TOTAL_BLOCKS / 32; i++) {
        uint32_t* ptr = (uint32_t*)&pmm_bitmap[i*4];
        if (*ptr != 0xFFFFFFFF) {
            for (int j = 0; j < 32; j++) {
                int bit = i * 32 + j;
                if (!mmap_test(bit))
                    return bit;
            }
        }
    }
    return -1;
}

void pmm_init(multiboot_info_t* mboot_info) {
    /* Initially mark EVERYTHING as used/reserved */
    memset(pmm_bitmap, 0xFF, BITMAP_SIZE);
    used_blocks = TOTAL_BLOCKS;

    /* Read the Memory Map from Multiboot to find actually free RAM */
    if (mboot_info->flags & (1 << 6)) { // Check if mmap is valid
        multiboot_memory_map_t* mmap = (multiboot_memory_map_t*)mboot_info->mmap_addr;
        
        while((uint32_t)mmap < mboot_info->mmap_addr + mboot_info->mmap_length) {
            
            // Type 1 = Available RAM
            if (mmap->type == 1) {
                uint64_t addr = mmap->addr;
                uint64_t len = mmap->len;

                // Mark these blocks as free in our bitmap
                for (uint64_t i = 0; i < len; i += BLOCK_SIZE) {
                    uint64_t frame_addr = addr + i;
                    uint32_t frame_idx = frame_addr / BLOCK_SIZE;
                    
                    if (frame_idx < TOTAL_BLOCKS) {
                        if (mmap_test(frame_idx)) { // If it was marked used
                            mmap_unset(frame_idx);
                            used_blocks--;
                        }
                    }
                }
            }
            mmap = (multiboot_memory_map_t*) ((unsigned int)mmap + mmap->size + sizeof(unsigned int));
        }
    }

    /* IMPORTANT: Mark the Kernel itself (and first 1MB) as USED! 
       Otherwise we might overwrite our own code. 
       Let's just reserve the first 2MB to be safe. */
    int kernel_pages = (2 * 1024 * 1024) / BLOCK_SIZE;
    for (int i = 0; i < kernel_pages; i++) {
        if (!mmap_test(i)) {
            mmap_set(i);
            used_blocks++;
        }
    }
}

void* pmm_alloc_block() {
    if (used_blocks >= max_blocks) return NULL; // Out of memory

    int frame = mmap_first_free();
    if (frame == -1) return NULL;

    mmap_set(frame);
    used_blocks++;
    
    uint32_t addr = frame * BLOCK_SIZE;
    return (void*)addr;
}

void pmm_free_block(void* p) {
    uint32_t addr = (uint32_t)p;
    int frame = addr / BLOCK_SIZE;
    
    if (mmap_test(frame)) {
        mmap_unset(frame);
        used_blocks--;
    }
}

uint32_t pmm_get_free_memory() {
    return (max_blocks - used_blocks) * BLOCK_SIZE;
}

uint32_t pmm_get_used_memory() {
    return used_blocks * BLOCK_SIZE;
}

uint32_t pmm_get_total_memory() {
    return max_blocks * BLOCK_SIZE;
}
