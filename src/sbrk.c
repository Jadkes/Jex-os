#include "pmm.h"
#include "paging.h"
#include <stdint.h>
#include <stddef.h>

static void* program_break = NULL;
static void* heap_end = NULL;

void* sbrk(intptr_t increment) {
    if (program_break == NULL) {
        // Initialize program break to start at 16MB
        program_break = (void*)0x1000000; 
        heap_end = program_break;
    }
    
    if (increment == 0) {
        return program_break;
    }
    
    void* old_break = program_break;
    
    if (increment > 0) {
        // Allocate more memory
        void* new_break = (void*)((uint32_t)program_break + increment);
        
        // Check if we need to allocate more physical pages
        while ((uint32_t)new_break > (uint32_t)heap_end) {
            void* new_frame = pmm_alloc_block();
            if (!new_frame) {
                return (void*)-1; // Out of memory
            }
            // Map the new frame to the virtual heap address
            map_page(new_frame, heap_end, 3); // User, RW, Present
            heap_end = (void*)((uint32_t)heap_end + 4096);
        }
        
        program_break = new_break;
    } else {
        // Shrink heap (simplified - just move break down)
        // We don't free physical pages here yet for simplicity
        uint32_t new_val = (uint32_t)program_break + increment;
        // Don't allow shrinking below initial break
        if (new_val < 0x1000000) {
            new_val = 0x1000000;
        }
        program_break = (void*)new_val;
    }
    
    return old_break;
}