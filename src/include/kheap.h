/**
 * @file kheap.h
 * @brief Kernel Heap Manager and standard string utilities.
 *
 * Provides dynamic memory allocation (kmalloc/kfree) for the kernel.
 */

#ifndef KHEAP_H
#define KHEAP_H

#include <stdint.h>
#include <stddef.h>

/**
 * @brief Allocate a block of memory from the kernel heap.
 * @param size Number of bytes to allocate.
 * @return Pointer to the allocated memory, or NULL if failed.
 */
void *kmalloc(size_t size);

/**
 * @brief Free a block of memory previously allocated by kmalloc.
 * @param p Pointer to the block to free.
 */
void kfree(void *p);

/**
 * @brief Initialize the kernel heap.
 * @param start_addr Starting physical address for the heap.
 */
void init_kheap(uint32_t start_addr);

/**
 * @brief Return number of bytes allocated from the heap so far.
 */
uint32_t kheap_get_used(void);

/**
 * @brief Return the total heap range start address.
 */
uint32_t kheap_get_start(void);

/**
 * @brief Return the current heap bump pointer (next free address).
 */
uint32_t kheap_get_current(void);

/* Standard Library Utility Functions */

void* memcpy(void* dest, const void* src, size_t n);
void* memset(void* s, int c, size_t n);
size_t strlen(const char* s);
int strcmp(const char* s1, const char* s2);
int strncmp(const char* s1, const char* s2, size_t n);
char* strcpy(char* dest, const char* src);
char* strcat(char* dest, const char* src);
char* strstr(const char* haystack, const char* needle);
char* strchr(const char* s, int c);
char* strrchr(const char* s, int c);
char* strncpy(char* dest, const char* src, size_t n);

#endif // KHEAP_H
