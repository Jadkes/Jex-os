#ifndef PAGING_H
#define PAGING_H

#include <stdint.h>
#include "isr.h"

// Page Table Entry (PTE)
typedef struct page_table_entry {
    uint32_t present    : 1;
    uint32_t rw         : 1;
    uint32_t user       : 1;
    uint32_t write_thru : 1;
    uint32_t cache_dis  : 1;
    uint32_t accessed   : 1;
    uint32_t dirty      : 1;
    uint32_t pat        : 1;
    uint32_t global     : 1;
    uint32_t avail      : 3;
    uint32_t frame      : 20;
} __attribute__((packed)) page_table_entry_t;

// Page Directory Entry (PDE)
typedef struct page_directory_entry {
    uint32_t present    : 1;
    uint32_t rw         : 1;
    uint32_t user       : 1;
    uint32_t write_thru : 1;
    uint32_t cache_dis  : 1;
    uint32_t accessed   : 1;
    uint32_t reserved   : 1;
    uint32_t page_size  : 1;
    uint32_t global     : 1;
    uint32_t avail      : 3;
    uint32_t table_frame: 20;
} __attribute__((packed)) page_directory_entry_t;

// Page Table (contains 1024 PTEs)
typedef struct page_table {
    page_table_entry_t pages[1024];
} __attribute__((aligned(4096))) page_table_t;

// Page Directory (contains 1024 PDEs)
typedef struct page_directory {
    page_directory_entry_t tables[1024];
} __attribute__((aligned(4096))) page_directory_t;

void init_paging();
void map_page(void* physaddr, void* virtualaddr, unsigned int flags);
void page_fault_handler(registers_t regs);

#endif