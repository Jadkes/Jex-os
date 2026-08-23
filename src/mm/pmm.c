/**
 * @file pmm.c
 * @brief Physical Memory Manager (PMM) implementation.
 *
 * Bitmap allocator for 4 KiB physical frames.  RAM detection comes from
 * the Multiboot memory map; kernel and bootloader regions are reserved.
 */

#define pr_fmt(fmt) "PMM: " fmt
#include "kernel/printk.h"
#include "spinlock.h"
#include "pmm.h"
#include "init.h"
#include "config.h"
#include <stddef.h>

extern void* memset(void* s, int c, size_t n);

/**
 * @brief Physical memory configuration.
 *
 * The bitmap spans the full 32-bit physical address space (4GB), so the
 * PMM can manage whatever RAM the bootloader reports — there is no
 * hardcoded 128MB cap.  Only frames actually listed as available by the
 * multiboot memory map are freed for allocation; everything else stays
 * reserved.
 *
 * 4GB / 4KB per block = 1048576 blocks.
 * 1048576 blocks / 8 blocks per byte = 131072 bytes (128KB) for the bitmap.
 */
#define FALLBACK_RAM_MB 128
#define BLOCKS_PER_BYTE 8
#define BLOCK_SIZE 4096
#define MAX_PHYS_BYTES 0x100000000ULL        /* full 32-bit address space */
#define TOTAL_BLOCKS (MAX_PHYS_BYTES / BLOCK_SIZE)
#define BITMAP_SIZE (TOTAL_BLOCKS / BLOCKS_PER_BYTE)

/*
 * Global bitmap: each bit represents one 4KB physical page.
 *   1 = Used/Reserved (or free after pmm_init)
 *   0 = Free (available for allocation)
 *
 * pmm_alloc_block() returns the physical address: frame × 4096.
 * used_blocks tracks the count of allocated frames.
 * free_blocks = TOTAL_BLOCKS - used_blocks.
 */
uint8_t pmm_bitmap[BITMAP_SIZE] __attribute__((aligned(4)));
uint32_t used_blocks = 0;
uint32_t max_blocks = TOTAL_BLOCKS;

/* Actual RAM detected from the bootloader (mmap, mem info, or fallback).
 * The bitmap spans 4GB of address space, so this is what 'free' reports
 * as total rather than the address-space span. */
static uint32_t total_mem_bytes = FALLBACK_RAM_MB * 1024 * 1024;

/**
 * @brief Mark a physical block as used in the bitmap.
 * @param bit The frame number (block index).
 */
static void bitmap_set(int bit) {
    pmm_bitmap[bit / 8] |= (1 << (bit % 8));
}

/**
 * @brief Mark a physical block as free in the bitmap.
 * @param bit The frame number (block index).
 */
static void bitmap_unset(int bit) {
    pmm_bitmap[bit / 8] &= ~(1 << (bit % 8));
}

/**
 * @brief Test if a physical block is marked as used.
 * @param bit The frame number (block index).
 * @return Non-zero if used, 0 if free.
 */
static int bitmap_test(int bit) {
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
static int bitmap_first_free() {
    for (uint32_t i = 0; i < TOTAL_BLOCKS / 32; i++) {
        uint32_t* ptr = (uint32_t*)&pmm_bitmap[i*4];
        /* If the 32-bit chunk is 0xFFFFFFFF, all 32 blocks are used */
        if (*ptr != 0xFFFFFFFF) {
            for (int j = 0; j < 32; j++) {
                int bit = i * 32 + j;
                if (!bitmap_test(bit))
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
static int bitmap_first_free_contiguous(uint32_t count) {
    if (count == 0) return -1;
    if (count == 1) return bitmap_first_free();

    for (uint32_t i = 0; i < TOTAL_BLOCKS; i++) {
        /* Check if the current block is free */
        if (!bitmap_test(i)) {
            uint32_t free_count = 0;
            /* Scan ahead to see if there's enough contiguous free space */
            for (uint32_t j = i; j < TOTAL_BLOCKS; j++) {
                if (!bitmap_test(j)) free_count++;
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

    /* Bytes of RAM the bootloader says is available. */
    uint64_t avail_bytes = 0;

    /* Process the Memory Map provided by the bootloader (e.g., GRUB) */
    if (mboot_info->flags & (1 << 6)) {
        multiboot_memory_map_t* mmap = (multiboot_memory_map_t*)mboot_info->mmap_addr;

        while((uint32_t)mmap < mboot_info->mmap_addr + mboot_info->mmap_length) {
            /* Type 1 = Available RAM. Other types are reserved or hardware mapped. */
            if (mmap->type == 1) {
                uint64_t addr = mmap->addr;
                uint64_t len = mmap->len;

                avail_bytes += len;

                /* Mark these ranges as free in our bitmap */
                for (uint64_t i = 0; i < len; i += BLOCK_SIZE) {
                    uint64_t frame_addr = addr + i;
                    uint32_t frame_idx = (uint32_t)(frame_addr / BLOCK_SIZE);

                    if (frame_idx < TOTAL_BLOCKS) {
                        /* Only unset if it was previously set (to avoid double counting) */
                        if (bitmap_test(frame_idx)) {
                            bitmap_unset(frame_idx);
                            used_blocks--;
                        }
                    }
                }
            }
            /* Advance to the next entry in the multiboot mmap */
            mmap = (multiboot_memory_map_t*) ((unsigned int)mmap + mmap->size + sizeof(unsigned int));
        }
    }

    /* Fallback: if the memory map freed nothing, fall back to the
     * multiboot mem_lower/mem_upper fields (bit 0), then to
     * FALLBACK_RAM_MB.  Covers bootloaders that report memory info
     * without an mmap (our custom stage-2 loader) and ones that report
     * neither. */
    if (used_blocks == TOTAL_BLOCKS) {
        if (mboot_info->flags & (1 << 0)) {
            uint32_t avail_kb = mboot_info->mem_lower + mboot_info->mem_upper;
            pr_info("no mmap, using multiboot mem info (%u KB)\n", avail_kb);
            avail_bytes = (uint64_t)avail_kb * 1024;
        } else {
            pr_info("no memory map from bootloader, assuming %dMB RAM\n",
                    FALLBACK_RAM_MB);
            avail_bytes = (uint64_t)FALLBACK_RAM_MB * 1024 * 1024;
        }

        uint32_t start_free = (2 * 1024 * 1024) / BLOCK_SIZE;  /* frame 512 */
        uint32_t end_free = (uint32_t)(avail_bytes / BLOCK_SIZE);
        if (end_free > TOTAL_BLOCKS)
            end_free = TOTAL_BLOCKS;
        for (uint32_t i = start_free; i < end_free; i++) {
            if (bitmap_test(i)) {
                bitmap_unset(i);
                used_blocks--;
            }
        }
    }

    total_mem_bytes = (uint32_t)(avail_bytes > 0xFFFFFFFFULL ? 0xFFFFFFFFULL : avail_bytes);

    /**
     * @brief Kernel Reservation.
     * We reserve the first 2MB to protect the Interrupt Vector Table (IVT),
     * BIOS data, and the kernel's own executable code and initial data structures.
     */
    int kernel_pages = (2 * 1024 * 1024) / BLOCK_SIZE;
    for (int i = 0; i < kernel_pages; i++) {
        if (!bitmap_test(i)) {
            bitmap_set(i);
            used_blocks++;
        }
    }

    /* Reserve bootloader-loaded modules (e.g. the GRUB RAM root image).
     * The mmap may list these as available; pin them so the heap never
     * hands the root filesystem out. */
    if (mboot_info->flags & (1 << 3)) {
        multiboot_module_t *mod = (multiboot_module_t *)mboot_info->mods_addr;
        for (uint32_t i = 0; i < mboot_info->mods_count; i++, mod++) {
            for (uint32_t a = mod->start; a < mod->end; a += BLOCK_SIZE) {
                uint32_t frame = a / BLOCK_SIZE;
                if (frame < TOTAL_BLOCKS && !bitmap_test(frame)) {
                    bitmap_set(frame);
                    used_blocks++;
                }
            }
        }
    }
}

/**
 * @brief Allocate a single 4KB block of physical RAM.
 * @return Physical address of the block, or NULL if OOM.
 */
/* Serializes PMM bitmap updates across CPUs */
static spinlock_t pmm_lock = SPINLOCK_INIT;

void* pmm_alloc_block() {
    uint32_t flags;
    spin_lock_irqsave(&pmm_lock, &flags);

    if (used_blocks >= max_blocks) {
        spin_unlock_irqrestore(&pmm_lock, flags);
        return NULL;
    }

    int frame = bitmap_first_free();
    if (frame == -1) {
        spin_unlock_irqrestore(&pmm_lock, flags);
        return NULL;
    }

    bitmap_set(frame);
    used_blocks++;
    spin_unlock_irqrestore(&pmm_lock, flags);

    return (void*)(frame * BLOCK_SIZE);
}

/**
 * @brief Allocate multiple contiguous 4KB blocks of physical RAM.
 * @param count Number of contiguous blocks to allocate.
 * @return Physical address of the first block, or NULL if OOM.
 */
void* pmm_alloc_blocks(uint32_t count) {
    uint32_t flags;
    spin_lock_irqsave(&pmm_lock, &flags);

    if (used_blocks + count > max_blocks) {
        spin_unlock_irqrestore(&pmm_lock, flags);
        return NULL;
    }

    int frame = bitmap_first_free_contiguous(count);
    if (frame == -1) {
        spin_unlock_irqrestore(&pmm_lock, flags);
        return NULL;
    }

    for (uint32_t i = 0; i < count; i++) {
        bitmap_set(frame + i);
    }
    used_blocks += count;
    spin_unlock_irqrestore(&pmm_lock, flags);

    return (void*)(frame * BLOCK_SIZE);
}

/**
 * @brief Free a 4KB block of physical RAM.
 * @param p Physical address to free.
 */
void pmm_free_block(void* p) {
    uint32_t addr = (uint32_t)p;
    uint32_t frame = addr / BLOCK_SIZE;

    /* Bounds check: reject addresses outside managed range */
    if (frame >= max_blocks) return;

    uint32_t flags;
    spin_lock_irqsave(&pmm_lock, &flags);
    if (bitmap_test(frame)) {
        bitmap_unset(frame);
        used_blocks--;
    }
    spin_unlock_irqrestore(&pmm_lock, flags);
}

/**
 * @brief Free multiple contiguous 4KB blocks of physical RAM.
 * @param p Physical address of the first block to free.
 * @param count Number of blocks to free.
 */
void pmm_free_blocks(void* p, uint32_t count) {
    uint32_t addr = (uint32_t)p;
    uint32_t frame = addr / BLOCK_SIZE;

    uint32_t flags;
    spin_lock_irqsave(&pmm_lock, &flags);
    for (uint32_t i = 0; i < count; i++) {
        if (frame + i >= max_blocks) break; /* Bounds check */
        if (bitmap_test(frame + i)) {
            bitmap_unset(frame + i);
            used_blocks--;
        }
    }
    spin_unlock_irqrestore(&pmm_lock, flags);
}

uint32_t pmm_get_free_memory() {
    return (max_blocks - used_blocks) * BLOCK_SIZE;
}

uint32_t pmm_get_used_memory() {
    uint32_t total = pmm_get_total_memory();
    uint32_t free = pmm_get_free_memory();
    return free < total ? total - free : 0;
}

uint32_t pmm_get_total_memory() {
    return total_mem_bytes;
}

/* Initcall wrapper: pmm_init needs mboot info passed via globals */
extern uint32_t g_mboot_magic;
extern multiboot_info_t* g_mboot_info;

static void pmm_wrapper(void) {
    if (g_mboot_magic == MULTIBOOT_MAGIC_VALID) {
        pmm_init(g_mboot_info);
    }
    else
        pr_err("Invalid Multiboot Magic, PMM init skipped!\n");
}
early_init(pmm_wrapper);
