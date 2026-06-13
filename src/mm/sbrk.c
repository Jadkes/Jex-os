/**
 * @file sbrk.c
 * @brief System Break (heap extension) implementation.
 *
 * Implements the sbrk syscall used by user-mode allocators (like malloc)
 * to increase their memory space.
 */

#include "pmm.h"
#include "paging.h"
#include <stdint.h>
#include <stddef.h>

/**
 * @brief Global pointers to track the current heap boundary.
 */
static void* program_break = NULL;
static void* heap_end = NULL;

/**
 * @brief Change the data segment size (extend/shrink the heap).
 * 
 * If increment is positive, this function allocates new physical pages
 * and maps them into the virtual address space.
 * 
 * @param increment Number of bytes to add/remove from the heap.
 * @return The old program break address on success, or (void*)-1 on failure.
 */
void* sbrk(intptr_t increment) {
    if (program_break == NULL) {
        /* Initialize program break to start at 16MB */
        program_break = (void*)0x1000000; 
        heap_end = program_break;
    }
    
    if (increment == 0) {
        return program_break;
    }
    
    void* old_break = program_break;
    
    if (increment > 0) {
        /* Move the break up */
        void* new_break = (void*)((uint32_t)program_break + increment);
        
        /* Check if the new break exceeds currently mapped pages */
        while ((uint32_t)new_break > (uint32_t)heap_end) {
            void* new_frame = pmm_alloc_block();
            if (!new_frame) {
                return (void*)-1; /* Out of physical memory */
            }
            /* Map the new physical frame to the virtual heap address */
            /* Flags: User-mode (3), Read/Write (2), Present (1) = 7 or 3? 
               Wait, map_page uses bitwise flags, but code used '3'. 
               Usually 1=Present, 2=RW, 4=User. 3=Present+RW. 7=Present+RW+User. */
            map_page(new_frame, heap_end, 7); /* Present + RW + User */
            heap_end = (void*)((uint32_t)heap_end + 4096);
        }
        
        program_break = new_break;
    } else {
        /* Shrink heap - move break down (simplification: don't free pages yet) */
        uint32_t new_val = (uint32_t)program_break + increment;
        /* Prevent shrinking below the start of the heap */
        if (new_val < 0x1000000) {
            new_val = 0x1000000;
        }
        program_break = (void*)new_val;
    }
    
    return old_break;
}
