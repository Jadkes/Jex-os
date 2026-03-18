/**
 * @file paging.h
 * @brief Virtual Memory Management (Paging).
 *
 * Defines structures for Page Directories and Page Tables for x86.
 */

#ifndef PAGING_H
#define PAGING_H

#include <stdint.h>
#include "isr.h"

/**
 * @struct page_table_entry
 * @brief x86 Page Table Entry (PTE) structure.
 */
typedef struct page_table_entry {
    uint32_t present    : 1;    /**< Page present in memory. */
    uint32_t rw         : 1;    /**< Read/Write (0=Read-only, 1=Read/Write). */
    uint32_t user       : 1;    /**< User/Supervisor (0=Supervisor only, 1=User). */
    uint32_t write_thru : 1;    /**< Write-through caching. */
    uint32_t cache_dis  : 1;    /**< Cache disabled. */
    uint32_t accessed   : 1;    /**< Set by CPU when page is accessed. */
    uint32_t dirty      : 1;    /**< Set by CPU when page is written to. */
    uint32_t pat        : 1;    /**< Page Attribute Table. */
    uint32_t global     : 1;    /**< Global page (prevent TLB flush). */
    uint32_t avail      : 3;    /**< Available for system use. */
    uint32_t frame      : 20;   /**< Top 20 bits of the physical address. */
} __attribute__((packed)) page_table_entry_t;

/**
 * @struct page_directory_entry
 * @brief x86 Page Directory Entry (PDE) structure.
 */
typedef struct page_directory_entry {
    uint32_t present    : 1;    /**< Page table present. */
    uint32_t rw         : 1;    /**< Read/Write. */
    uint32_t user       : 1;    /**< User/Supervisor. */
    uint32_t write_thru : 1;    /**< Write-through caching. */
    uint32_t cache_dis  : 1;    /**< Cache disabled. */
    uint32_t accessed   : 1;    /**< Set by CPU when accessed. */
    uint32_t reserved   : 1;    /**< Reserved (must be 0). */
    uint32_t page_size  : 1;    /**< Page size (0=4KB, 1=4MB). */
    uint32_t global     : 1;    /**< Ignored. */
    uint32_t avail      : 3;    /**< Available for system use. */
    uint32_t table_frame: 20;   /**< Top 20 bits of the physical address of the page table. */
} __attribute__((packed)) page_directory_entry_t;

/**
 * @struct page_table
 * @brief Represents a single page table (1024 entries).
 */
typedef struct page_table {
    page_table_entry_t pages[1024];
} __attribute__((aligned(4096))) page_table_t;

/**
 * @struct page_directory
 * @brief Represents a page directory (1024 entries).
 */
typedef struct page_directory {
    page_directory_entry_t tables[1024];
} __attribute__((aligned(4096))) page_directory_t;

/**
 * @brief Initialize paging and identity map the kernel.
 */
void init_paging();

/**
 * @brief Map a physical address to a virtual address.
 * 
 * @param physaddr The physical address.
 * @param virtualaddr The virtual address.
 * @param flags The mapping flags (Present, RW, User, etc.).
 */
void map_page(void* physaddr, void* virtualaddr, unsigned int flags);

/**
 * @brief Handler for Page Fault exceptions (Interrupt 14).
 * @param regs The CPU state at the time of the fault.
 */
void page_fault_handler(registers_t regs);

#endif // PAGING_H
