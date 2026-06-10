/**
 * @file paging.c
 * @brief Virtual Memory Management (Paging).
 *
 * Implements x86 two-level paging (Page Directory and Page Tables).
 * Handles virtual-to-physical mapping and Page Fault exceptions.
 */

#include "paging.h"
#include "pmm.h"
#include "isr.h"
#include "kheap.h"
#include "terminal.h"
#include "panic.h"
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

/**
 * @brief Global kernel page directory.
 */
page_directory_t kernel_directory;

/**
 * @brief Get or create a page table for a specific directory index.
 * 
 * @param table_idx Index in the page directory (0-1023).
 * @param flags Flags to apply to the directory entry.
 * @return Pointer to the existing or newly allocated page table.
 */
static page_table_t* get_table(uint32_t table_idx, uint32_t flags) {
    if (kernel_directory.tables[table_idx].present) {
        page_table_t* table = (page_table_t*)(kernel_directory.tables[table_idx].table_frame << 12);
        if ((flags & 0x4) && !kernel_directory.tables[table_idx].user) {
            kernel_directory.tables[table_idx].user = 1;
            kernel_directory.tables[table_idx].rw = (flags & 0x2) ? 1 : 0;
        }
        return table;
    }

    /* Allocate a new page table if not present */
    page_table_t* new_table = (page_table_t*)pmm_alloc_block();
    if (!new_table) return NULL;

    memset(new_table, 0, sizeof(page_table_t));

    kernel_directory.tables[table_idx].table_frame = ((uint32_t)new_table) >> 12;
    kernel_directory.tables[table_idx].present = (flags & 0x1) ? 1 : 0;
    kernel_directory.tables[table_idx].rw = (flags & 0x2) ? 1 : 0;
    kernel_directory.tables[table_idx].user = (flags & 0x4) ? 1 : 0;

    return new_table;
}

/**
 * @brief Map a physical frame to a virtual address.
 * 
 * @param physaddr The physical address.
 * @param virtualaddr The virtual address.
 * @param flags PTE flags (Present, RW, User).
 */
void map_page(void* physaddr, void* virtualaddr, unsigned int flags) {
    uint32_t pdindex = (uint32_t)virtualaddr >> 22;
    uint32_t ptindex = ((uint32_t)virtualaddr >> 12) & 0x03FF;

    page_table_t* table = get_table(pdindex, flags);
    if (!table) return;

    table->pages[ptindex].frame = (uint32_t)physaddr >> 12;
    table->pages[ptindex].present = (flags & 0x1) ? 1 : 0;
    table->pages[ptindex].rw = (flags & 0x2) ? 1 : 0;
    table->pages[ptindex].user = (flags & 0x4) ? 1 : 0;
}

/**
 * @brief Unmap a virtual address by clearing its page table entry.
 *
 * @param virtualaddr Virtual address to unmap.
 */
void paging_unmap_page(void* virtualaddr) {
    uint32_t pdindex = (uint32_t)virtualaddr >> 22;
    uint32_t ptindex = ((uint32_t)virtualaddr >> 12) & 0x03FF;

    if (!kernel_directory.tables[pdindex].present)
        return;

    page_table_t* table = (page_table_t*)((uint32_t)kernel_directory.tables[pdindex].table_frame << 12);
    table->pages[ptindex].present = 0;

    /* Flush TLB entry for this page */
    asm volatile("invlpg (%0)" :: "r"(virtualaddr));
}

/**
 * @brief Initialize paging and identity map the first 512MB of RAM.
 * Loads the page directory into CR3 and enables the PG bit in CR0.
 */
void init_paging() {
    memset(&kernel_directory, 0, sizeof(kernel_directory));

    /**
     * @brief Identity Mapping.
     * Maps the first 512MB (0-0x20000000) so that Virtual Address == Physical Address.
     * This is necessary for the kernel to continue running immediately after paging is enabled.
     */
    const uint32_t num_pages = (512u * 1024u * 1024u) / 4096u;
    const unsigned int kernel_flags = 0x3; /* Present + RW (Kernel) */

    for (uint32_t i = 0; i < num_pages; i++) {
        void* phys = (void*)(i * 4096u);
        void* virt = phys;
        map_page(phys, virt, kernel_flags);
    }

    /* Load the Page Directory into CR3 */
    asm volatile("mov %0, %%cr3" :: "r"(&kernel_directory));

    /* Enable Paging (Set Bit 31 of CR0) */
    uint32_t cr0;
    asm volatile("mov %%cr0, %0" : "=r"(cr0));
    cr0 |= 0x80000000;
    asm volatile("mov %0, %%cr0" :: "r"(cr0));
    
    terminal_writestring("Paging enabled (Identity Map 512MB).\n");
}

/**
 * @brief Page Fault Exception Handler (Interrupt 14).
 * Decodes the faulting address and error code to provide diagnostic output.
 */
void page_fault_handler(registers_t regs) {
    /* Delegate to the new panic handler for full diagnostic output */
    panic_handler(&regs);
    for (;;) asm volatile("hlt");
}
