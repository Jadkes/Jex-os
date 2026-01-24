#include "../include/stdlib.h"
#include "../include/jexos.h"
#include "../include/string.h"

typedef struct block {
    size_t size;
    struct block* next;
    int free;
} block_t;

#define BLOCK_HEADER_SIZE sizeof(block_t)
static block_t* heap_list = NULL;

void* malloc(size_t size) {
    if (size == 0) return NULL;
    
    // Align size to 8 bytes
    size = (size + 7) & ~7;
    
    block_t* prev = NULL;
    block_t* curr = heap_list;
    
    // First fit search
    while (curr) {
        if (curr->free && curr->size >= size) {
            // Found a free block
            curr->free = 0;
            
            // Split block if it's much larger than needed
            if (curr->size >= size + BLOCK_HEADER_SIZE + 8) {
                block_t* new_block = (block_t*)((char*)curr + BLOCK_HEADER_SIZE + size);
                new_block->size = curr->size - size - BLOCK_HEADER_SIZE;
                new_block->free = 1;
                new_block->next = curr->next;
                
                curr->size = size;
                curr->next = new_block;
            }
            
            return (char*)curr + BLOCK_HEADER_SIZE;
        }
        prev = curr;
        curr = curr->next;
    }
    
    // No suitable block found, request more memory
    void* ptr = sys_sbrk(size + BLOCK_HEADER_SIZE);
    if (ptr == (void*)-1) return NULL;
    
    block_t* new_block = (block_t*)ptr;
    new_block->size = size;
    new_block->free = 0;
    new_block->next = NULL;
    
    if (prev) {
        prev->next = new_block;
    } else {
        heap_list = new_block;
    }
    
    return (char*)new_block + BLOCK_HEADER_SIZE;
}

void free(void* ptr) {
    if (!ptr) return;
    
    block_t* block = (block_t*)((char*)ptr - BLOCK_HEADER_SIZE);
    block->free = 1;
    
    // Coalesce with next block if it's also free
    if (block->next && block->next->free) {
        block->size += block->next->size + BLOCK_HEADER_SIZE;
        block->next = block->next->next;
    }
    
    // Coalesce with previous block if it exists and is free
    block_t* curr = heap_list;
    while (curr && curr->next != block) {
        curr = curr->next;
    }
    if (curr && curr->free) {
        curr->size += block->size + BLOCK_HEADER_SIZE;
        curr->next = block->next;
    }
}

void exit(int status) {
    (void)status; // Status ignored for now
    sys_exit();
}

int atoi(const char* str) {
    int result = 0;
    int sign = 1;
    
    // Skip whitespace
    while (*str == ' ' || *str == '\t' || *str == '\n' || *str == '\r') {
        str++;
    }
    
    // Handle sign
    if (*str == '-') {
        sign = -1;
        str++;
    } else if (*str == '+') {
        str++;
    }
    
    // Convert digits
    while (*str >= '0' && *str <= '9') {
        result = result * 10 + (*str - '0');
        str++;
    }
    
    return sign * result;
}