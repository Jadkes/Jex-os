/**
 * @file pmm.h
 * @brief Physical Memory Manager (PMM).
 *
 * Manages physical memory blocks (4KB pages) using a bitmap.
 */

#ifndef PMM_H
#define PMM_H

#include <stdint.h>
#include "multiboot.h"

/**
 * @brief Size of a physical memory block (4KB).
 */
#define PMM_BLOCK_SIZE 4096

/**
 * @brief Initialize the physical memory manager using Multiboot memory map.
 * 
 * @param mboot_info Pointer to the multiboot information structure.
 * @return void
 */
void pmm_init(multiboot_info_t* mboot_info);

/**
 * @brief Allocate a single 4KB block of physical memory.
 * 
 * @return Physical address of the allocated block, or NULL if out of memory.
 */
void* pmm_alloc_block();

/**
 * @brief Allocate multiple contiguous 4KB blocks of physical memory.
 * 
 * @param count Number of contiguous blocks to allocate.
 * @return Physical address of the first allocated block, or NULL if out of memory.
 */
void* pmm_alloc_blocks(uint32_t count);

/**
 * @brief Free a previously allocated physical memory block.
 * 
 * @param p Physical address of the block to free.
 * @return void
 */
void pmm_free_block(void* p);

/**
 * @brief Free multiple contiguous 4KB blocks of physical memory.
 * 
 * @param p Physical address of the first block to free.
 * @param count Number of blocks to free.
 * @return void
 */
void pmm_free_blocks(void* p, uint32_t count);

/**
 * @brief Get the amount of free physical memory in bytes.
 * 
 * @return Number of free bytes.
 */
uint32_t pmm_get_free_memory();

/**
 * @brief Get the amount of used physical memory in bytes.
 * 
 * @return Number of used bytes.
 */
uint32_t pmm_get_used_memory();

/**
 * @brief Get the total amount of physical memory detected.
 * 
 * @return Total number of bytes.
 */
uint32_t pmm_get_total_memory();

#endif // PMM_H
