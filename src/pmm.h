#ifndef PMM_H
#define PMM_H

#include <stdint.h>
#include "multiboot.h"

#define PMM_BLOCK_SIZE 4096 // 4KB

void pmm_init(multiboot_info_t* mboot_info);
void* pmm_alloc_block();
void pmm_free_block(void* p);
uint32_t pmm_get_free_memory();
uint32_t pmm_get_used_memory();
uint32_t pmm_get_total_memory();

#endif
