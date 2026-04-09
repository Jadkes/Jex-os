/**
 * @file pmm.c
 * @brief Physical Memory Manager (PMM) implementation.
 *
 * This file manages the physical allocation of 4KB RAM blocks using a bitmap.
 * It uses the Multiboot memory map to detect available RAM and reserve
 * sensitive areas (like the kernel itself).
 */

#include "pmm.h"
#include <stddef.h>

extern void* memset(void* s, int c, size_t n);

/**
 * @brief Memory Management configuration.
 * Currently limited to 128MB for simplicity and to keep the bitmap at 4KB.
 * 
 * 128MB / 4KB per block = 32768 blocks.
 * 32768 blocks / 8 blocks per byte = 4096 bytes (4KB) for the bitmap.
 */
#define RAM_SIZE_MB 128
#define BLOCKS_PER_BYTE 8
#define BLOCK_SIZE 4096
#define TOTAL_BLOCKS (RAM_SIZE_MB * 1024 * 1024 / BLOCK_SIZE)
#define BITMAP_SIZE (TOTAL_BLOCKS / BLOCKS_PER_BYTE)

/* Global bitmap: each bit represents one 4KB physical page. 
 * 1 = Used/Reserved, 0 = Free.
 */
uint8_t pmm_bitmap[BITMAP_SIZE];
uint32_t used_blocks = 0;
uint32_t max_blocks = TOTAL_BLOCKS;

/**
 * @brief Mark a physical block as used in the bitmap.
 * @param bit The block index.
 */
static void mmap_set(int bit) {
    pmm_bitmap[bit / 8] |= (1 << (bit % 8));
}

/**
 * @brief Mark a physical block as free in the bitmap.
 * @param bit The block index.
 */
static void mmap_unset(int bit) {
    pmm_bitmap[bit / 8] &= ~(1 << (bit % 8));
}

/**
 * @brief Test if a physical block is marked as used.
 * @param bit The block index.
 * @return Non-zero if used, 0 if free.
 */
static int mmap_test(int bit) {
    return pmm_bitmap[bit / 8] & (1 << (bit % 8));
}

/**
 * @brief Find the first available (free) block in the bitmap.
 * 
 * Optimized to check 32-bit chunks before bit-level scanning.
 * This significantly speeds up allocation when memory is mostly free.
 * 
 * @return The index of the first free block, or -1 if none found.
 */
static int mmap_first_free() {
    for (uint32_t i = 0; i < TOTAL_BLOCKS / 32; i++) {
        uint32_t* ptr = (uint32_t*)&pmm_bitmap[i*4];
        /* If the 32-bit chunk is 0xFFFFFFFF, all 32 blocks are used */
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

/**
 * @brief Find the first available contiguous blocks of memory.
 * 
 * Required for DMA buffers where hardware needs physically contiguous RAM.
 * 
 * @param count Number of contiguous blocks to find.
 * @return The index of the first free block, or -1 if none found.
 */
static int mmap_first_free_s(uint32_t count) {
    if (count == 0) return -1;
    if (count == 1) return mmap_first_free();

    for (uint32_t i = 0; i < TOTAL_BLOCKS; i++) {
        /* Check if the current block is free */
        if (!mmap_test(i)) {
            uint32_t free_count = 0;
            /* Scan ahead to see if there's enough contiguous free space */
            for (uint32_t j = i; j < TOTAL_BLOCKS; j++) {
                if (!mmap_test(j)) free_count++;
                else {
                    /* Not enough space here, skip to after the blocked page */
                    i = j; 
                    break;
                }

                if (free_count == count) return i;
            }
        }
    }
    return -1;
}

/**
 * @brief Initialize the PMM using Multiboot memory information.
 * 
 * Marks all memory as reserved initially, then frees regions reported
 * as 'Available' by the BIOS/Bootloader. Finally, re-reserves the first 2MB 
 * to protect the kernel and BIOS structures.
 * 
 * @param mboot_info Pointer to Multiboot structure.
 */
void pmm_init(multiboot_info_t* mboot_info) {
    /* Initially mark EVERYTHING as used/reserved for safety */
    memset(pmm_bitmap, 0xFF, BITMAP_SIZE);
    used_blocks = TOTAL_BLOCKS;

    /* Process the Memory Map provided by the bootloader (e.g., GRUB) */
    if (mboot_info->flags & (1 << 6)) {
        multiboot_memory_map_t* mmap = (multiboot_memory_map_t*)mboot_info->mmap_addr;
        
        while((uint32_t)mmap < mboot_info->mmap_addr + mboot_info->mmap_length) {
            /* Type 1 = Available RAM. Other types are reserved or hardware mapped. */
            if (mmap->type == 1) {
                uint64_t addr = mmap->addr;
                uint64_t len = mmap->len;

                /* Mark these ranges as free in our bitmap */
                for (uint64_t i = 0; i < len; i += BLOCK_SIZE) {
                    uint64_t frame_addr = addr + i;
                    uint32_t frame_idx = (uint32_t)(frame_addr / BLOCK_SIZE);
                    
                    if (frame_idx < TOTAL_BLOCKS) {
                        /* Only unset if it was previously set (to avoid double counting) */
                        if (mmap_test(frame_idx)) {
                            mmap_unset(frame_idx);
                            used_blocks--;
                        }
                    }
                }
            }
            /* Advance to the next entry in the multiboot mmap */
            mmap = (multiboot_memory_map_t*) ((unsigned int)mmap + mmap->size + sizeof(unsigned int));
        }
    }

    /**
     * @brief Kernel Reservation.
     * We reserve the first 2MB to protect the Interrupt Vector Table (IVT), 
     * BIOS data, and the kernel's own executable code and initial data structures.
     */
    int kernel_pages = (2 * 1024 * 1024) / BLOCK_SIZE;
    for (int i = 0; i < kernel_pages; i++) {
        if (!mmap_test(i)) {
            mmap_set(i);
            used_blocks++;
        }
    }
}

/**
 * @brief Allocate a single 4KB block of physical RAM.
 * @return Physical address of the block, or NULL if OOM.
 */
void* pmm_alloc_block() {
    if (used_blocks >= max_blocks) return NULL;

    int frame = mmap_first_free();
    if (frame == -1) return NULL;

    mmap_set(frame);
    used_blocks++;
    
    return (void*)(frame * BLOCK_SIZE);
}

/**
 * @brief Allocate multiple contiguous 4KB blocks of physical RAM.
 * @param count Number of contiguous blocks to allocate.
 * @return Physical address of the first block, or NULL if OOM.
 */
void* pmm_alloc_blocks(uint32_t count) {
    if (used_blocks + count > max_blocks) return NULL;

    int frame = mmap_first_free_s(count);
    if (frame == -1) return NULL;

    for (uint32_t i = 0; i < count; i++) {
        mmap_set(frame + i);
    }
    used_blocks += count;

    return (void*)(frame * BLOCK_SIZE);
}

/**
 * @brief Free a 4KB block of physical RAM.
 * @param p Physical address to free.
 */
void pmm_free_block(void* p) {
    uint32_t addr = (uint32_t)p;
    int frame = addr / BLOCK_SIZE;
    
    if (mmap_test(frame)) {
        mmap_unset(frame);
        used_blocks--;
    }
}

/**
 * @brief Free multiple contiguous 4KB blocks of physical RAM.
 * @param p Physical address of the first block to free.
 * @param count Number of blocks to free.
 */
void pmm_free_blocks(void* p, uint32_t count) {
    uint32_t addr = (uint32_t)p;
    int frame = addr / BLOCK_SIZE;

    for (uint32_t i = 0; i < count; i++) {
        if (mmap_test(frame + i)) {
            mmap_unset(frame + i);
            used_blocks--;
        }
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
