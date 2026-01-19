#include "paging.h"
#include "pmm.h"
#include "isr.h"
#include <stddef.h>

extern void terminal_writestring(const char* data);
extern void terminal_putchar(char c);

/* 
   We allocate the directory statically. 
   Tables will be allocated dynamically using pmm_alloc_block().
*/
page_directory_t kernel_directory;

/* Helper to get a page table, creating it if necessary */
page_table_t* get_table(int table_idx) {
    if (kernel_directory.tables[table_idx].present) {
        return (page_table_t*)(kernel_directory.tables[table_idx].table_frame << 12);
    }

    /* Allocate a new table using PMM */
    page_table_t* new_table = (page_table_t*)pmm_alloc_block();
    
    /* Clear the new table (very important!) */
    uint32_t* ptr = (uint32_t*)new_table;
    for(int i = 0; i < 1024; i++) ptr[i] = 0;

    /* Register it in the directory */
    kernel_directory.tables[table_idx].table_frame = ((uint32_t)new_table) >> 12;
    kernel_directory.tables[table_idx].present = 1;
    kernel_directory.tables[table_idx].rw = 1;
    kernel_directory.tables[table_idx].user = 0;

    return new_table;
}

void init_paging()
{
    /* 1. Clear the directory */
    uint32_t* dir_ptr = (uint32_t*)&kernel_directory;
    for(int i = 0; i < 1024; i++) dir_ptr[i] = 0;

    /* 2. Identity map the first 128MB (0x0 - 0x08000000) */
    /* This covers the kernel, VGA, and the Heap */
    uint32_t num_pages = (128 * 1024 * 1024) / 4096;
    
    for (uint32_t i = 0; i < num_pages; i++) {
        uint32_t address = i * 4096;
        int table_idx = i / 1024;
        int page_idx = i % 1024;

        page_table_t* table = get_table(table_idx);
        
        table->pages[page_idx].present = 1;
        table->pages[page_idx].rw = 1;
        table->pages[page_idx].user = 1; /* ALLOW USER MODE */
        table->pages[page_idx].frame = i;
    }

    /* Update directory entries too */
    for(int i = 0; i < 1024; i++) {
        if(kernel_directory.tables[i].present) {
            kernel_directory.tables[i].user = 1;
        }
    }

    /* 3. Load CR3 with the address of the page directory */
    asm volatile("mov %0, %%cr3" :: "r"(&kernel_directory));

    /* 4. Enable Paging! */
    uint32_t cr0;
    asm volatile("mov %%cr0, %0": "=r"(cr0));
    cr0 |= 0x80000000;
    asm volatile("mov %0, %%cr0":: "r"(cr0));
    
    terminal_writestring("Paging enabled (Identity Map 128MB).\n");
}

void page_fault_handler(registers_t regs)
{
    uint32_t faulting_address;
    asm volatile("mov %%cr2, %0" : "=r" (faulting_address));

    terminal_writestring("\nPAGE FAULT detected!\n");
    terminal_writestring("Address: ");
    
    char *digits = "0123456789ABCDEF";
    terminal_writestring("0x");
    for (int i = 28; i >= 0; i -= 4) {
        terminal_putchar(digits[(faulting_address >> i) & 0xF]);
    }
    terminal_writestring("\n");

    int present = !(regs.err_code & 0x1);
    int rw = regs.err_code & 0x2;
    int us = regs.err_code & 0x4;

    terminal_writestring("Reason: ");
    if (present) terminal_writestring("Not Present ");
    if (rw) terminal_writestring("Write ");
    if (us) terminal_writestring("User-Mode ");
    terminal_writestring("\nSystem Halted.\n");
    for(;;)
        asm volatile("hlt");
}