/**
 * @file paging.c
 * @brief Virtual Memory Management (Paging).
 *
 * Implements x86 two-level paging (Page Directory and Page Tables).
 * Handles virtual-to-physical mapping and Page Fault exceptions.
 */

#include "kernel/printk.h"
#include "paging.h"
#include "pmm.h"
#include "init.h"
#include "isr.h"
#include "kheap.h"
#include "terminal.h"
#include "panic.h"
#include "task.h"
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

/**
 * @brief Global kernel page directory.
 */
page_directory_t kernel_directory;

/**
 * @brief Helper: Return the page directory for the current context.
 * If multitasking is active and the current task has its own page directory,
 * use that; otherwise use the kernel directory (boot/init phase).
 */
static page_directory_t* current_page_dir(void) {
    if (current_task && current_task->page_directory)
        return (page_directory_t*)current_task->page_directory;
    return &kernel_directory;
}

/**
 * @brief Get or create a page table in a specific directory.
 */
static page_table_t* get_table_in(page_directory_t* dir, uint32_t table_idx, uint32_t flags) {
    if (dir->tables[table_idx].present) {
        page_table_t* table = (page_table_t*)(dir->tables[table_idx].table_frame << 12);
        if ((flags & 0x4) && !dir->tables[table_idx].user) {
            dir->tables[table_idx].user = 1;
            dir->tables[table_idx].rw = (flags & 0x2) ? 1 : 0;
        }
        return table;
    }

    /* Allocate a new page table if not present */
    page_table_t* new_table = (page_table_t*)pmm_alloc_block();
    if (!new_table) return NULL;

    memset(new_table, 0, sizeof(page_table_t));

    dir->tables[table_idx].table_frame = ((uint32_t)new_table) >> 12;
    dir->tables[table_idx].present = (flags & 0x1) ? 1 : 0;
    dir->tables[table_idx].rw = (flags & 0x2) ? 1 : 0;
    dir->tables[table_idx].user = (flags & 0x4) ? 1 : 0;

    return new_table;
}

/**
 * @brief Get or create a page table for the current context's directory.
 */
static page_table_t* get_table(uint32_t table_idx, uint32_t flags) {
    return get_table_in(current_page_dir(), table_idx, flags);
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

    page_directory_t* dir = current_page_dir();

    if (!dir->tables[pdindex].present)
        return;

    page_table_t* table = (page_table_t*)((uint32_t)dir->tables[pdindex].table_frame << 12);
    table->pages[ptindex].present = 0;

    /* Flush TLB entry for this page */
    asm volatile("invlpg (%0)" :: "r"(virtualaddr));
}

/**
 * @brief Map a page in a specific page directory.
 * Like map_page() but targets an explicit directory instead of the current one.
 */
void map_page_in(page_directory_t* dir, void* physaddr, void* virtualaddr, unsigned int flags) {
    uint32_t pdindex = (uint32_t)virtualaddr >> 22;
    uint32_t ptindex = ((uint32_t)virtualaddr >> 12) & 0x03FF;

    page_table_t* table = get_table_in(dir, pdindex, flags);
    if (!table) return;

    table->pages[ptindex].frame = (uint32_t)physaddr >> 12;
    table->pages[ptindex].present = (flags & 0x1) ? 1 : 0;
    table->pages[ptindex].rw = (flags & 0x2) ? 1 : 0;
    table->pages[ptindex].user = (flags & 0x4) ? 1 : 0;
}

/**
 * @brief Clone a page directory, deep-copying user pages and sharing kernel pages.
 *
 * Creates a new page directory with separate physical frames for all user-mapped
 * pages (content copied). Kernel pages are shared (same physical frames).
 * Used by fork() to give each process an independent address space.
 *
 * @return Pointer to the new page directory (physical address, identity-mapped).
 */
page_directory_t* clone_page_directory(page_directory_t* src) {
    void* new_dir_frame = pmm_alloc_block();
    if (!new_dir_frame) return NULL;
    page_directory_t* dst = (page_directory_t*)new_dir_frame;
    memset(dst, 0, sizeof(page_directory_t));

    for (int i = 0; i < 1024; i++) {
        if (!src->tables[i].present)
            continue;

        uint32_t* src_table = (uint32_t*)((uint32_t)src->tables[i].table_frame << 12);

        if (src->tables[i].user) {
            /* User page table — clone every page */
            void* new_table_frame = pmm_alloc_block();
            if (!new_table_frame) return NULL;
            uint32_t* dst_table = (uint32_t*)new_table_frame;

            for (int j = 0; j < 1024; j++) {
                uint32_t src_pte = src_table[j];
                if (!(src_pte & 1))
                    continue;
                if (src_pte & 4) {
                    /* User page — allocate new frame and copy contents */
                    void* new_page = pmm_alloc_block();
                    if (!new_page) return NULL;
                    memcpy(new_page, (void*)(src_pte & 0xFFFFF000), 4096);
                    dst_table[j] = (src_pte & 0xFFF) | ((uint32_t)new_page & 0xFFFFF000);
                } else {
                    /* Kernel page in same table — share */
                    dst_table[j] = src_pte;
                }
            }

            dst->tables[i].table_frame = (uint32_t)new_table_frame >> 12;
            dst->tables[i].present = 1;
            dst->tables[i].rw = src->tables[i].rw;
            dst->tables[i].user = 1;
            dst->tables[i].write_thru = src->tables[i].write_thru;
            dst->tables[i].cache_dis = src->tables[i].cache_dis;
            dst->tables[i].page_size = src->tables[i].page_size;
        } else {
            /* Kernel page table — share the whole table */
            dst->tables[i] = src->tables[i];
        }
    }

    return dst;
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
    
    pr_info("Paging enabled (Identity Map 512MB).\n");
}

/**
 * @brief Page Fault Exception Handler (Interrupt 14).
 * Decodes the faulting address and error code to provide diagnostic output.
 */
early_init(init_paging);

void page_fault_handler(registers_t regs) {
    /* Delegate to the new panic handler for full diagnostic output */
    panic_handler(&regs);
    for (;;) asm volatile("hlt");
}
