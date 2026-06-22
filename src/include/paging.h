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
 * @defgroup recursive_paging Recursive Page Table Access (PDE[1022])
 *
 * PDE[1022] self-references the page directory, giving a virtual window
 * into ALL page tables at fixed addresses — no identity-mapped access
 * needed.  Used for all PTE/PDE reads and writes to prevent identity-map
 * aliasing bugs (page table frames accidentally written through PDE[0-127]).
 *
 * Typical use:
 *   page_table_entry_t* pt = PTE_PTR(pde_idx);   // read/write PTE[i][j]
 *   page_directory_entry_t* pde = PDE_PTR(idx);  // read/write PDE[idx]
 *   scratch_map_frame(phys);                      // temp map a physical frame
 *   // ... write through SCRATCH_PTE_BASE ...
 *   scratch_unmap();
 *
 * @{
 */
#define RECURSIVE_PDE_IDX     1022
#define RECURSIVE_PDE_BASE    0xFF800000u      /* RECURSIVE_PDE_IDX << 22            */
#define RECURSIVE_PDE_SELF    0xFFBFE000u      /* PD self: base + 1022*0x1000        */
                                              /* (PT_idx = 1022 hits PDE[1022]'s   */
                                              /*  table_frame = PD - offset = PD)   */
#define SCRATCH_PDE_IDX       1021
#define SCRATCH_PTE_BASE      0xFF400000u      /* SCRATCH_PDE_IDX << 22              */

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

/** Get a writable pointer to the whole page table for @p pde_idx */
#define PTE_PTR(pde_idx) \
    ((page_table_entry_t*)(RECURSIVE_PDE_BASE + ((pde_idx) << 12)))

/** Get a writable pointer to PDE @p pde_idx itself */
#define PDE_PTR(pde_idx) \
    ((page_directory_entry_t*)(RECURSIVE_PDE_SELF + ((pde_idx) << 2)))

/** @brief Temporarily map a physical frame at SCRATCH_PTE_BASE.
 *
 * Uses a single 32-bit store for the PDE to avoid the GCC packed-bitfield
 * read-modify-write hazard (separate writes can zero adjacent bits). */
static inline void scratch_map_frame(uint32_t phys_addr) {
    uint32_t* raw = (uint32_t*)PDE_PTR(SCRATCH_PDE_IDX);
    *raw = ((phys_addr >> 12) << 12)  /* table_frame */
         | (1u << 0)                  /* present */
         | (1u << 1);                 /* rw */
    /* user = 0 (kernel only) */
    __asm__ volatile("invlpg (%0)" :: "r"(SCRATCH_PTE_BASE));
}

/** @brief Unmap the scratch window after use. */
static inline void scratch_unmap(void) {
    /* Clear PTE[0] self-map while scratch is still accessible */
    *(uint32_t*)SCRATCH_PTE_BASE = 0;
    *(uint32_t*)PDE_PTR(SCRATCH_PDE_IDX) = 0;
    __asm__ volatile("invlpg (%0)" :: "r"(SCRATCH_PTE_BASE));
}
/** @} */

/**
 * @brief Clear the identity-mapped PTE for a physical address.
 *
 * After a physical frame is allocated for use as a page table, calling
 * this prevents any kernel code from accidentally writing to the page
 * table through the identity map (PDE 0-127), which caused the fork
 * PTE corruption bug.
 *
 * @param phys Physical address whose identity PTE to clear.
 */
/** @brief Check if paging (CR0.PG) is active. */
static inline int paging_is_active(void) {
    uint32_t cr0;
    __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
    return (cr0 & 0x80000000) ? 1 : 0;
}

static inline void identity_unmap_frame(uint32_t phys) {
    /* Without paging there is no identity map to clear */
    if (!paging_is_active()) return;

    uint32_t pdindex = phys >> 22;
    if (pdindex > 127) return;          /* not in identity map */
    page_table_entry_t* id_pt = PTE_PTR(pdindex);
    uint32_t ptindex = (phys >> 12) & 0x3FF;
    id_pt[ptindex].present = 0;
    __asm__ volatile("invlpg (%0)" :: "r"(phys));
}

/* Kernel stack virtual address base — PDE 1023, shared page table */
#define KSTACK_VADDR_BASE  0xFFC00000u
#define KSTACK_SIZE        8192
#define KSTACK_PDE_IDX     1023

/**
 * alloc_kernel_stack - Allocate 8KB physical RAM and map as kernel stack.
 * @pid: Task PID determines slot: KSTACK_VADDR_BASE + pid * KSTACK_SIZE.
 * @return: Stack base virtual address, or 0 on failure.
 */
uint32_t alloc_kernel_stack(int pid);

/**
 * free_kernel_stack - Unmap and free physical pages for a kernel stack.
 * @pid: Task PID whose stack to free.
 */
void free_kernel_stack(int pid);

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
int map_page(void* physaddr, void* virtualaddr, unsigned int flags);

/**
 * @brief Map a page in a specific page directory (for fork/clone).
 */
int map_page_in(page_directory_t* dir, void* physaddr, void* virtualaddr, unsigned int flags);

/**
 * @brief Clone a page directory, deep-copying user pages.
 * @return New page directory with independent user frames.
 */
page_directory_t* clone_page_directory(page_directory_t* src);

/**
 * @brief Unmap a virtual page (mark as not present).
 *
 * @param virtualaddr The virtual address of the page to unmap.
 */
void paging_unmap_page(void* virtualaddr);

/**
 * @brief Handler for Page Fault exceptions (Interrupt 14).
 * @param regs The CPU state at the time of the fault.
 */
void page_fault_handler(registers_t* regs);

/**
 * @brief Debug dump of the PTE for a virtual address (serial only).
 * @param vaddr The virtual address to dump PTE for.
 */
void dump_pte(void* vaddr);

#endif // PAGING_H
