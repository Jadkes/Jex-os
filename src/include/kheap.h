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

/* =============== Slab Allocator =============== */

#define SLAB_MAGIC      0x534C4142  /* "SLAB" */
#define LARGE_MAGIC     0x4C524700  /* "LRG\0" */
#define SLAB_PAGE_SIZE  4096
#define SLAB_MAX_SIZE   2048
#define SLAB_CACHE_COUNT 8

/* Per-object state lives in a bitmap after the header.  The free-list
 * alone cannot detect freeing the SAME object twice (the over-free
 * count check only trips when every object is already free), and one
 * duplicated index silently aliases two allocations.  32 bytes covers
 * the largest slab this allocator can build (254 x 16B objects). */
#define SLAB_BITMAP_BYTES 32

typedef struct slab {
    uint32_t        magic;      /* SLAB_MAGIC or LARGE_MAGIC */
    struct slab*    next;       /* Next slab in cache's linked list */
    uint32_t        obj_size;   /* Size of each object in bytes */
    uint32_t        obj_count;  /* Total objects in this slab */
    uint32_t        free_count; /* Number of free objects remaining */
    uint32_t        free_head;  /* Index of first free object (0xFFFFFFFF = empty) */
    uint8_t         inuse[SLAB_BITMAP_BYTES]; /* 1 = allocated, 0 = free */
} __attribute__((packed)) slab_t;

typedef struct {
    slab_t*     slab_list;      /* Linked list of all slabs in this cache */
    uint32_t    obj_size;       /* Object size for this cache */
    uint32_t    objs_per_slab;  /* Precomputed: (4096 - sizeof(slab_t)) / obj_size */
} slab_cache_t;

typedef struct {
    uint32_t magic;             /* LARGE_MAGIC */
    uint32_t pages;             /* Number of 4KB pages allocated */
} large_hdr_t;

/* Heap API */
void init_kheap(void);
uint32_t kheap_get_free(void);
void     kheap_reclaim(void);

/* Legacy API */
void *kmalloc(size_t size);
void kfree(void *p);
uint32_t kheap_get_used(void);
uint32_t kheap_get_start(void);
uint32_t kheap_get_current(void);

/* Standard Library Utility Functions */

void* memcpy(void* dest, const void* src, size_t n);
void* memset(void* s, int c, size_t n);
size_t strlen(const char* s);
int strcmp(const char* s1, const char* s2);
int memcmp(const void* a1, const void* a2, size_t n);
int strncmp(const char* s1, const char* s2, size_t n);
char* strcpy(char* dest, const char* src);
char* strcat(char* dest, const char* src);
char* strstr(const char* haystack, const char* needle);
char* strchr(const char* s, int c);
char* strrchr(const char* s, int c);
char* strncpy(char* dest, const char* src, size_t n);

#endif // KHEAP_H
