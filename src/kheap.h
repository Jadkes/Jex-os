#ifndef KHEAP_H
#define KHEAP_H

#include <stdint.h>
#include <stddef.h>

/* Allocate memory from the heap */
void *kmalloc(size_t size);

/* Free memory (not implemented in this simple version, 
   but we'll add the stub for future use) */
void kfree(void *p);

/* Initialize heap with a start address */
void init_kheap(uint32_t start_addr);

/* Memory utilities */
void* memcpy(void* dest, const void* src, size_t n);
void* memset(void* s, int c, size_t n);
size_t strlen(const char* s);
char* strcpy(char* dest, const char* src);
char* strcat(char* dest, const char* src);
char* strstr(const char* haystack, const char* needle);
char* strrchr(const char* s, int c);

#endif