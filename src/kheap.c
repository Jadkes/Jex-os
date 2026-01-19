#include "kheap.h"

/* 
   Simple placement allocator.
   In a more advanced version, we would use a proper heap with free lists.
   But for a FAT12 driver and basic shell, this is very stable.
*/

uint32_t placement_address;

void init_kheap(uint32_t start_addr) {
    placement_address = start_addr;
}

void *kmalloc(size_t size) {
    /* Align to 4 bytes */
    if (placement_address & 0xFFF) {
        placement_address &= 0xFFFFF000;
        placement_address += 0x1000;
    }
    
    void *tmp = (void *)placement_address;
    placement_address += size;
    return tmp;
}

void kfree(void *p) {
    /* For now, we don't free in a placement allocator. 
       This is 'Linus style' for early boot/simple kernels. */
    (void)p;
}
