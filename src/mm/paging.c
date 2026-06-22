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
 *
 * Returns a pointer to the page table via the recursive mapping window
 * (for the current PD when paging is active) or scratch window (non-current PD).
 * When a new page table is allocated during init_paging (paging not yet active),
 * returns the physical address directly — PTE_PTR only works after paging is on.
 *
 * Every newly allocated page table has PTE[0] self-mapped so the scratch
 * window (PDE[1021]) can read/write the frame through virtual 0xFF400000.
 * identity_unmap_frame is called after PTE[0] setup, so the identity PTE
 * is always clear — no aliasing possible.
 */
static page_table_t* get_table_in(page_directory_t* dir, uint32_t table_idx, uint32_t flags) {
    int is_current = (dir == current_page_dir());

    if (dir->tables[table_idx].present) {
        /*
         * If the caller wants user access but the PDE is supervisor-only
         * (e.g. identity-mapped PDE[32] / PDE[1] created with user=0 during
         * boot), update the PDE flags.  Individual PTE user bits still gate
         * each page, so setting PDE user=1 does NOT expose kernel pages in
         * the same 4MB range to user mode.
         */
        if ((flags & 0x4) && !dir->tables[table_idx].user)
            dir->tables[table_idx].user = 1;

        if (is_current) {
            if (paging_is_active())
                return (page_table_t*)PTE_PTR(table_idx);
            else
                return (page_table_t*)(dir->tables[table_idx].table_frame << 12);
        } else {
            /* Non-current PD: scratch-map the page table frame.
             * PTE[0] is self-mapped (set during initial allocation),
             * so 0xFF400000 is accessible through the scratch window. */
            uint32_t pt_phys = dir->tables[table_idx].table_frame << 12;
            scratch_map_frame(pt_phys);
            return (page_table_t*)SCRATCH_PTE_BASE;
        }
    }

    /* Allocate a new page table if not present */
    void* new_pt = pmm_alloc_block();
    if (!new_pt) return NULL;

    /* Zero through identity map (still present at this point) */
    memset(new_pt, 0, sizeof(page_table_t));
    uint32_t pt_phys = (uint32_t)new_pt;

    /*
     * Self-map PTE[0] so the scratch window (PDE[1021]) can access this
     * page table frame at virtual 0xFF400000.  Without this self-map,
     * PTE[0] is zeroed and accessing 0xFF400000 would page-fault even
     * though PDE[1021] is present.
     *
     * MUST use a single 32-bit store — separate bitfield writes on packed
     * structs cause GCC to zero adjacent bits (read-modify-write hazard).
     *
     * Written through identity map (still present here).
     */
    {
        uint32_t pte_val = ((pt_phys >> 12) << 12)  /* frame */
                         | (1u << 0)                  /* present */
                         | (1u << 1);                 /* rw */
        *(uint32_t*)pt_phys = pte_val;
    }

    /* Clear identity PTE — no aliasing possible after this */
    identity_unmap_frame(pt_phys);

    /* Set PDE in the directory — single 32-bit store to avoid GCC
     * bitfield RMW hazard on packed structs. */
    {
        uint32_t pde_val = ((pt_phys >> 12) << 12)
                         | ((flags & 0x1) ? 1u : 0u)
                         | ((flags & 0x2) ? 2u : 0u)
                         | ((flags & 0x4) ? 4u : 0u);
        *(uint32_t*)&dir->tables[table_idx] = pde_val;
    }

    /* Return via recursive window (current PD) or scratch (non-current).
     * Before init_paging turns paging on, return the physical address —
     * PTE_PTR/recursive window only works with paging active. */
    if (is_current) {
        if (paging_is_active()) {
            /* Flush TLB entry for the new PDE so recursive window sees it */
            uint32_t dummy = RECURSIVE_PDE_BASE + (table_idx << 12);
            __asm__ volatile("invlpg (%0)" :: "r"(dummy));
            return (page_table_t*)PTE_PTR(table_idx);
        } else {
            return (page_table_t*)pt_phys;
        }
    } else {
        scratch_map_frame(pt_phys);
        return (page_table_t*)SCRATCH_PTE_BASE;
    }
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

    /* Single 32-bit store to avoid GCC bitfield RMW hazard */
    uint32_t pte_val = (((uint32_t)physaddr >> 12) << 12)  /* frame */
                     | ((flags & 0x1) ? 1u : 0u)            /* present */
                     | ((flags & 0x2) ? 2u : 0u)            /* rw */
                     | ((flags & 0x4) ? 4u : 0u);           /* user */
    *(uint32_t*)&table->pages[ptindex] = pte_val;
}

/**
 * @brief Unmap a virtual address by clearing its page table entry.
 *
 * @param virtualaddr Virtual address to unmap.
 */
void paging_unmap_page(void* virtualaddr) {
    uint32_t pdindex = (uint32_t)virtualaddr >> 22;
    uint32_t ptindex = ((uint32_t)virtualaddr >> 12) & 0x03FF;

    if (!current_page_dir()->tables[pdindex].present)
        return;

    page_table_t* table = (page_table_t*)PTE_PTR(pdindex);
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

    /* Single 32-bit store to avoid GCC bitfield RMW hazard */
    {
        uint32_t pte_val = (((uint32_t)physaddr >> 12) << 12)
                         | ((flags & 0x1) ? 1u : 0u)
                         | ((flags & 0x2) ? 2u : 0u)
                         | ((flags & 0x4) ? 4u : 0u);
        *(uint32_t*)&table->pages[ptindex] = pte_val;
    }
}

/**
 * @brief Clone a page directory, deep-copying user pages and sharing kernel pages.
 *
 * Creates a new page directory with separate physical frames for all user-mapped
 * pages (content copied). Kernel pages are shared (same physical frames).
 * Used by fork() to give each process an independent address space.
 *
 * Access pattern: src page tables read through recursive mapping window
 * (PTE_PTR), dst page tables written through scratch window (SCRATCH_PTE_BASE).
 * Every new page table frame has PTE[0] self-mapped so the scratch window can
 * access it.  identity_unmap is called after PTE[0] setup, so no identity-map
 * aliasing is possible for any page table frame.
 *
 * @return Pointer to the new page directory (physical address).
 */
page_directory_t* clone_page_directory(page_directory_t* src) {
    void* new_dir_frame = pmm_alloc_block();
    if (!new_dir_frame) return NULL;
    page_directory_t* dst = (page_directory_t*)new_dir_frame;
    memset(dst, 0, sizeof(page_directory_t));

    /* Track allocated page table frames for error cleanup */
    uint32_t alloc_pt_frames[1024];
    memset(alloc_pt_frames, 0, sizeof(alloc_pt_frames));

    for (int i = 0; i < 1024; i++) {
        if (!src->tables[i].present)
            continue;

        if (src->tables[i].user || i == 32) {
            /* User or user-code page table — clone every page.
             * PDE[32] (0x08000000-0x083FFFFF, TCC code) is always deep-copied
             * to ensure every task owns its page table — no sharing with the
             * kernel's identity-mapped PDE[32]. */

            /* Allocate new page table frame */
            void* new_pt = pmm_alloc_block();
            if (!new_pt) goto oom_cleanup;
            memset(new_pt, 0, 4096);
            uint32_t pt_phys = (uint32_t)new_pt;

            /*
             * Self-map PTE[0] so the scratch window can access this frame.
             * But we DON'T use the scratch window for copying — the copy
             * loop would overwrite PTE[0] (j=0 writes to dst_pt[0] which
             * IS PTE[0]), breaking the self-map for j>=1.
             *
             * Instead, we write the destination page table directly through
             * the identity map.  The identity PTE for pt_phys is re-established
             * if it was previously cleared by identity_unmap_frame.
             *
             * Single 32-bit store to avoid GCC bitfield RMW hazard.
             */
            {
                /* Re-establish identity PTE for pt_phys if it was cleared. */
                uint32_t pdindex = pt_phys >> 22;
                uint32_t ptindex = (pt_phys >> 12) & 0x3FF;
                page_table_entry_t* id_pt = PTE_PTR(pdindex);
                uint32_t id_pte_val = ((pt_phys >> 12) << 12)
                                    | (1u << 0)  /* present */
                                    | (1u << 1); /* rw */
                *(uint32_t*)&id_pt[ptindex] = id_pte_val;
                __asm__ volatile("invlpg (%0)" :: "r"(pt_phys));

                /* Set PTE[0] self-map (needed by scratch_map_frame later
                 * for OTHER page tables, not for this copy). */
                uint32_t pte_val = ((pt_phys >> 12) << 12)
                                 | (1u << 0)
                                 | (1u << 1);
                *(uint32_t*)pt_phys = pte_val;
                /* Leave identity PTE present — we need it for the copy below. */
            }

            alloc_pt_frames[i] = pt_phys;

            /* Read src page table through recursive window (src == current PD) */
            page_table_entry_t* src_pt = PTE_PTR(i);

            /* Write dst page table DIRECTLY through the identity map.
             * Don't use the scratch window — the copy loop would overwrite
             * PTE[0] (the self-map), breaking all subsequent accesses. */
            uint32_t* dst_pt_raw = (uint32_t*)pt_phys;

            for (int j = 0; j < 1024; j++) {
                uint32_t src_pte_raw = *(uint32_t*)&src_pt[j];
                if (!(src_pte_raw & 1))
                    continue;
                if (src_pte_raw & 4) {
                    /* User page — allocate new frame and copy contents */
                    void* new_page = pmm_alloc_block();
                    if (!new_page) goto oom_cleanup;
                    memcpy(new_page, (void*)(src_pte_raw & 0xFFFFF000), 4096);
                    uint32_t new_phys = (uint32_t)new_page;
                    uint32_t new_pte = (src_pte_raw & 0xFFF) | (new_phys & 0xFFFFF000);
                    dst_pt_raw[j] = new_pte;
                } else {
                    /* Kernel page in same table — share */
                    dst_pt_raw[j] = src_pte_raw;
                }
            }

            /* PTE[0] was overwritten by the copy loop (if source PTE[0]
             * was present).  Re-establish the self-map so the scratch
             * window works for future calls. */
            {
                uint32_t pte_val = ((pt_phys >> 12) << 12)
                                 | (1u << 0)
                                 | (1u << 1);
                *(uint32_t*)pt_phys = pte_val;
            }

            /*
             * Build PDE as a single 32-bit value to prevent GCC from emitting
             * separate byte writes for adjacent bitfields.  With separate
             * writes GCC can (and does) zero previously-set bits: the byte
             * write for "user" overwrites the present bit set by an earlier
             * 32-bit rmw for "table_frame | present".  This single-assignment
             * pattern guarantees a single 32-bit store with all flags set.
             */
            uint32_t frame_num = pt_phys >> 12;
            uint32_t pde_val = (frame_num << 12) | 1;                   /* frame + present */
            if (src->tables[i].rw)      pde_val |= (1u << 1);          /* rw */
            pde_val |= (1u << 2);                                       /* user */
            if (src->tables[i].write_thru) pde_val |= (1u << 3);       /* write_thru */
            if (src->tables[i].cache_dis)  pde_val |= (1u << 4);       /* cache_dis */
            if (src->tables[i].page_size)  pde_val |= (1u << 7);       /* page_size */
            memcpy(&dst->tables[i], &pde_val, sizeof(pde_val));
        } else {
            /* Kernel page table — share the whole table */
            dst->tables[i] = src->tables[i];
        }
    }

    /* Set recursive PDE[1022] in destination — points to dst itself.
     * MUST use a single 32-bit store, not separate bitfield writes,
     * because GCC can zero adjacent bits with separate byte writes
     * (same bug pattern as the deep-copy PDE construction above). */
    {
        uint32_t dst_phys = (uint32_t)dst;
        uint32_t pde_val = ((dst_phys >> 12) << 12)  /* table_frame */
                         | (1u << 0)                  /* present */
                         | (1u << 1);                 /* rw */
        /* user = 0 (kernel only) */
        memcpy(&dst->tables[RECURSIVE_PDE_IDX], &pde_val, sizeof(pde_val));
    }

    return dst;

oom_cleanup:
    /* Free all allocated page table frames.
     * User pages within partially-built tables are leaked — acceptable
     * for OOM (system is out of memory and will likely halt anyway). */
    for (int i = 0; i < 1024; i++) {
        if (alloc_pt_frames[i])
            pmm_free_block((void*)(uint32_t)alloc_pt_frames[i]);
    }
    pmm_free_block(new_dir_frame);
    return NULL;
}

/**
 * @brief Initialize paging and identity map the first 512MB of RAM.
 * Loads the page directory into CR3 and enables the PG bit in CR0.
 *
 * Sets PDE[1022] as the recursive mapping (self-reference to the page directory)
 * so all page table access goes through the virtual window at 0xFF800000+
 * instead of through identity-mapped physical addresses.
 */
void init_paging() {
    memset(&kernel_directory, 0, sizeof(kernel_directory));

    /**
     * @brief Identity Mapping.
     * Maps the first 512MB (0-0x20000000) so that Virtual Address == Physical Address.
     * This is necessary for the kernel to continue running immediately after paging is enabled.
     *
     * During this loop paging is NOT yet active, so get_table_in returns physical
     * addresses.  identity_unmap_frame is a no-op until paging is on — harmless.
     */
    const uint32_t num_pages = (512u * 1024u * 1024u) / 4096u;
    const unsigned int kernel_flags = 0x3; /* Present + RW (Kernel) */

    for (uint32_t i = 0; i < num_pages; i++) {
        void* phys = (void*)(i * 4096u);
        void* virt = phys;
        map_page(phys, virt, kernel_flags);
    }

    /* Set the recursive PDE[1022] to point to kernel_directory itself.
     * Single 32-bit store to avoid GCC bitfield RMW hazard. */
    {
        uint32_t pde_val = (((uint32_t)&kernel_directory >> 12) << 12)
                         | (1u << 0)  /* present */
                         | (1u << 1); /* rw */
        *(uint32_t*)&kernel_directory.tables[RECURSIVE_PDE_IDX] = pde_val;
    }

    /* Load the Page Directory into CR3 */
    asm volatile("mov %0, %%cr3" :: "r"(&kernel_directory));

    /* Enable Paging (Set Bit 31 of CR0) */
    uint32_t cr0;
    asm volatile("mov %%cr0, %0" : "=r"(cr0));
    cr0 |= 0x80000000;
    asm volatile("mov %0, %%cr0" :: "r"(cr0));

    pr_info("Paging enabled (identity 512MB + recursive mapping PDE[1022]).\n");

    /* Allocate page table for PDE[1023] — dedicated to kernel stacks */
    page_table_t* kstack_pt = (page_table_t*)pmm_alloc_block();
    if (!kstack_pt) {
        for (;;) asm volatile("hlt");
    }
    memset(kstack_pt, 0, sizeof(page_table_t));
    uint32_t kstack_pt_phys = (uint32_t)kstack_pt;

    /* Self-map PTE[0] for scratch window compatibility.
     * Single 32-bit store to avoid GCC bitfield RMW hazard. */
    {
        uint32_t pte_val = ((kstack_pt_phys >> 12) << 12)
                         | (1u << 0)  /* present */
                         | (1u << 1); /* rw */
        *(uint32_t*)kstack_pt_phys = pte_val;
    }

    /* Set PDE[1023] to point to the kernel stack page table.
     * Single 32-bit store to avoid GCC bitfield RMW hazard. */
    {
        uint32_t pde_val = ((kstack_pt_phys >> 12) << 12)
                         | (1u << 0)  /* present */
                         | (1u << 1); /* rw */
        *(uint32_t*)&kernel_directory.tables[KSTACK_PDE_IDX] = pde_val;
    }

    /* Clear identity PTE for the kernel stack page table frame — it's a page table,
     * and identity-map writes to it would corrupt kernel stacks.
     * paging_is_active() is true here, so identity_unmap_frame works. */
    identity_unmap_frame(kstack_pt_phys);

    /* Flush TLB for the recursive window so PDE[1022] takes effect */
    {
        uint32_t v = RECURSIVE_PDE_BASE;
        __asm__ volatile("invlpg (%0)" :: "r"(v));
    }
}

/**
 * alloc_kernel_stack - Allocate 2 PMM pages and map at KSTACK_VADDR_BASE + pid*8KB.
 * @pid: Task PID for slot selection (slot = pid * 2 PTEs).
 * @return: Stack base virtual address, or 0 on failure.
 *
 * Physical pages are PMM-allocated and their PTEs written into PDE 1023's
 * shared page table (kernel_directory).  Since PDE 1023 is kernel-shared
 * (user=0, shared across all page directories via clone_page_directory),
 * every kernel stack is reachable from any CR3 — no special-casing in
 * fork or context switch needed.
 *
 * The kernel stack page table is accessed through the recursive window
 * (PTE_PTR), NOT through identity-mapped physical addresses, since the
 * page table frame's identity PTE is cleared during init_paging.
 */
uint32_t alloc_kernel_stack(int pid)
{
    if (!kernel_directory.tables[KSTACK_PDE_IDX].present)
        return 0;

    page_table_t* kstack_pt = (page_table_t*)PTE_PTR(KSTACK_PDE_IDX);
    uint32_t slot_idx = pid * (KSTACK_SIZE / 4096);   /* pid * 2 */

    /* Bail if slot already occupied */
    for (int i = 0; i < 2; i++) {
        if (kstack_pt->pages[slot_idx + i].present)
            return 0;
    }

    /* Allocate and map both 4KB pages */
    for (int i = 0; i < 2; i++) {
        void* phys = pmm_alloc_block();
        if (!phys) {
            /* Roll back on failure */
            for (int j = 0; j < i; j++) {
                uint32_t p = kstack_pt->pages[slot_idx + j].frame << 12;
                pmm_free_block((void*)p);
                kstack_pt->pages[slot_idx + j].present = 0;
            }
            return 0;
        }
        /* Single 32-bit store to avoid GCC bitfield RMW hazard */
        uint32_t pte_val = (((uint32_t)phys >> 12) << 12)  /* frame */
                         | (1u << 0)                         /* present */
                         | (1u << 1);                        /* rw */
        /* user = 0 (kernel only) */
        *(uint32_t*)&kstack_pt->pages[slot_idx + i] = pte_val;
    }

    return KSTACK_VADDR_BASE + pid * KSTACK_SIZE;
}

/**
 * free_kernel_stack - Unmap and free the 2 physical pages of a kernel stack.
 * @pid: Task PID of the stack to free.
 *
 * Clears both PTEs in PDE 1023's shared page table and returns the physical
 * pages to PMM.  Invalidates TLB entries for both pages.
 */
void free_kernel_stack(int pid)
{
    if (!kernel_directory.tables[KSTACK_PDE_IDX].present)
        return;

    page_table_t* kstack_pt = (page_table_t*)PTE_PTR(KSTACK_PDE_IDX);
    uint32_t slot_idx = pid * (KSTACK_SIZE / 4096);

    for (int i = 0; i < 2; i++) {
        if (kstack_pt->pages[slot_idx + i].present) {
            uint32_t phys = kstack_pt->pages[slot_idx + i].frame << 12;
            pmm_free_block((void*)phys);
            kstack_pt->pages[slot_idx + i].present = 0;
        }
    }

    /* Flush TLB for both pages */
    asm volatile("invlpg (%0)" :: "r"(KSTACK_VADDR_BASE + pid * KSTACK_SIZE));
    asm volatile("invlpg (%0)" :: "r"(KSTACK_VADDR_BASE + pid * KSTACK_SIZE + 4096));
}

/**
 * @brief Debug dump of PTE for a given virtual address.
 * Reads the PTE from the current page directory through the recursive
 * mapping window.  Logs to serial.  Used for debugging exec/page-fault issues.
 */
void dump_pte(void* vaddr) {
    extern void log_serial(const char* s);
    extern void log_hex_serial(uint32_t n);
    page_directory_t* dir = current_page_dir();
    uint32_t pdindex = (uint32_t)vaddr >> 22;
    uint32_t ptindex = ((uint32_t)vaddr >> 12) & 0x3FF;

    log_serial("[PTEDUMP] vaddr=");
    log_hex_serial((uint32_t)vaddr);
    log_serial(" PDE[");
    log_hex_serial(pdindex);
    log_serial("]=");
    { uint32_t raw; memcpy(&raw, &dir->tables[pdindex], sizeof(raw)); log_hex_serial(raw); }
    log_serial(" (present=");
    log_hex_serial(dir->tables[pdindex].present);
    log_serial(" user=");
    log_hex_serial(dir->tables[pdindex].user);
    log_serial(")\n");

    if (dir->tables[pdindex].present) {
        page_table_entry_t* pt = PTE_PTR(pdindex);
        log_serial("  PTE[");
        log_hex_serial(ptindex);
        log_serial("]=");
        log_hex_serial(*(uint32_t*)&pt[ptindex]);
        log_serial(" (present=");
        log_hex_serial(pt[ptindex].present);
        log_serial(" user=");
        log_hex_serial(pt[ptindex].user);
        log_serial(" frame=0x");
        log_hex_serial(pt[ptindex].frame);
        log_serial(")\n");
    }
}

/**
 * @brief Page Fault Exception Handler (Interrupt 14).
 * Decodes the faulting address and error code to provide diagnostic output.
 */
early_init(init_paging);

void page_fault_handler(registers_t* regs) {
    extern void log_serial(const char* s);
    extern void log_hex_serial(uint32_t n);
    uint32_t cr2;
    __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));
    uint32_t pdindex = cr2 >> 22;
    page_directory_entry_t* pde = &current_page_dir()->tables[pdindex];

    log_serial("CR2=");
    log_hex_serial(cr2);
    log_serial(" PDE[");
    log_hex_serial(pdindex);
    log_serial("]=");
    log_hex_serial(*(uint32_t*)pde);
    log_serial(" (user=");
    log_hex_serial(pde->user);
    log_serial(" present=");
    log_hex_serial(pde->present);
    log_serial(")\n");

    if (pde->present) {
        page_table_entry_t* pt = PTE_PTR(pdindex);
        uint32_t ptindex = (cr2 >> 12) & 0x3FF;
        page_table_entry_t* pte = &pt[ptindex];
        log_serial("  PTE[");
        log_hex_serial(ptindex);
        log_serial("]=");
        log_hex_serial(*(uint32_t*)pte);
        log_serial(" (user=");
        log_hex_serial(pte->user);
        log_serial(" present=");
        log_hex_serial(pte->present);
        log_serial(" frame=0x");
        log_hex_serial(pte->frame);
        log_serial(")\n");
    }

    panic_handler(regs);
    for (;;) asm volatile("hlt");
}
