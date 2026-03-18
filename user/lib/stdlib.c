/**
 * @file stdlib.c
 * @brief Standard library implementation (Memory allocation, etc.).
 */

#include "../include/stdlib.h"
#include "../include/jexos.h"
#include "../include/string.h"

/**
 * @struct block
 * @brief Meta-data header for heap-allocated blocks.
 */
typedef struct block {
    size_t size;        /**< Size of the usable data area. */
    struct block* next; /**< Link to the next block in the heap. */
    int free;           /**< Flag: 1 if block is available for use. */
} block_t;

#define BLOCK_HEADER_SIZE sizeof(block_t)
static block_t* heap_list = NULL;

/**
 * @brief First-fit dynamic memory allocator.
 */
void* malloc(size_t size) {
    if (size == 0) return NULL;
    
    /* Align size to 8 bytes for efficiency */
    size = (size + 7) & ~7;
    
    block_t* prev = NULL;
    block_t* curr = heap_list;
    
    /* Search for an existing free block */
    while (curr) {
        if (curr->free && curr->size >= size) {
            curr->free = 0;
            
            /* Split the block if it's much larger than requested */
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
    
    /* No suitable block found, extend the heap via sbrk */
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

/**
 * @brief Mark a heap block as free.
 */
void free(void* ptr) {
    if (!ptr) return;
    
    block_t* block = (block_t*)((char*)ptr - BLOCK_HEADER_SIZE);
    block->free = 1;
    
    /* Coalesce with next block if free */
    if (block->next && block->next->free) {
        block->size += block->next->size + BLOCK_HEADER_SIZE;
        block->next = block->next->next;
    }
    
    /* Simple previous coalesce: scan from start */
    block_t* curr = heap_list;
    while (curr && curr->next != block) {
        curr = curr->next;
    }
    if (curr && curr->free) {
        curr->size += block->size + BLOCK_HEADER_SIZE;
        curr->next = block->next;
    }
}

/**
 * @brief Exit the process.
 */
void exit(int status) {
    (void)status;
    sys_exit();
}

/**
 * @brief Convert string to integer.
 */
int atoi(const char* str) {
    int result = 0;
    int sign = 1;
    
    /* Skip whitespace */
    while (*str == ' ' || *str == '\t' || *str == '\n' || *str == '\r') {
        str++;
    }
    
    /* Handle signs */
    if (*str == '-') {
        sign = -1;
        str++;
    } else if (*str == '+') {
        str++;
    }
    
    /* Accumulate digits */
    while (*str >= '0' && *str <= '9') {
        result = result * 10 + (*str - '0');
        str++;
    }
    
    return sign * result;
}
