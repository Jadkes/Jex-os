/**
 * @file kheap.c
 * @brief Kernel Heap — power-of-2 slab allocator with PMM fallback.
 *
 * 8 size classes (16B–2048B). Intra-slab free list for O(1) alloc/free.
 * Slab header at page start for O(1) kfree via page-mask + magic check.
 * Allocations > 2048 bytes go to PMM with LARGE_MAGIC header.
 * String utilities in the second half of the file are kept for linking.
 */

#include "kheap.h"
#include "pmm.h"
#include "init.h"
#include <stddef.h>
#include <stdint.h>

static const uint32_t slab_sizes[SLAB_CACHE_COUNT] = {
    16, 32, 64, 128, 256, 512, 1024, 2048
};

static slab_cache_t caches[SLAB_CACHE_COUNT];
static uint32_t total_committed = 0;   /* All slab pages + PMM large allocs */

static void init_caches(void) {
    for (int i = 0; i < SLAB_CACHE_COUNT; i++) {
        caches[i].obj_size = slab_sizes[i];
        caches[i].slab_list = NULL;
        caches[i].objs_per_slab =
            (SLAB_PAGE_SIZE - sizeof(slab_t)) / slab_sizes[i];
    }
}

static slab_t* slab_create(int cache_idx) {
    slab_cache_t* cache = &caches[cache_idx];

    slab_t* slab = (slab_t*)pmm_alloc_blocks(1);
    if (!slab) return NULL;

    slab->magic = SLAB_MAGIC;
    slab->next = NULL;
    slab->obj_size = cache->obj_size;
    slab->obj_count = cache->objs_per_slab;
    slab->free_count = cache->objs_per_slab;
    slab->free_head = 0;

    /* Build intra-slab free list via indices stored in free object memory */
    uint8_t* data = (uint8_t*)((uint32_t)slab + sizeof(slab_t));
    for (uint32_t i = 0; i < slab->obj_count; i++) {
        uint32_t* next_idx = (uint32_t*)(data + i * slab->obj_size);
        *next_idx = (i == slab->obj_count - 1) ? 0xFFFFFFFF : (i + 1);
    }

    total_committed += SLAB_PAGE_SIZE;
    return slab;
}

static void* slab_alloc(int cache_idx) {
    slab_cache_t* cache = &caches[cache_idx];
    slab_t* slab = cache->slab_list;

    /* Walk the list to find a slab with free objects */
    while (slab && slab->free_count == 0) {
        slab = slab->next;
    }

    /* No space — allocate a new slab from PMM */
    if (!slab) {
        slab = slab_create(cache_idx);
        if (!slab) return NULL;
        slab->next = cache->slab_list;
        cache->slab_list = slab;
    }

    /* Pop from free list: object stores index of next free object */
    uint8_t* data = (uint8_t*)((uint32_t)slab + sizeof(slab_t));
    uint32_t idx = slab->free_head;
    uint32_t* obj = (uint32_t*)(data + idx * slab->obj_size);
    slab->free_head = *obj;
    slab->free_count--;

    return (void*)obj;
}

static void slab_free(slab_t* slab, void* ptr) {
    uint8_t* data = (uint8_t*)((uint32_t)slab + sizeof(slab_t));
    uint32_t offset = (uint32_t)ptr - (uint32_t)data;
    uint32_t idx = offset / slab->obj_size;

    /* Push onto free list */
    uint32_t* obj = (uint32_t*)(data + idx * slab->obj_size);
    *obj = slab->free_head;
    slab->free_head = idx;
    slab->free_count++;
}

static slab_t* ptr_to_slab(void* ptr) {
    slab_t* slab = (slab_t*)((uint32_t)ptr & ~0xFFF);
    if (slab->magic == SLAB_MAGIC || slab->magic == LARGE_MAGIC)
        return slab;
    return NULL;
}

/**
 * @brief Allocate kernel memory.
 *
 * Routes to slab cache for sizes <= SLAB_MAX_SIZE (2048), PMM large alloc for larger.
 * Returns NULL for size == 0 or allocation failure.
 *
 * @param size Number of bytes to allocate.
 * @return Pointer to allocated memory, or NULL.
 */
void* kmalloc(size_t size) {
    if (size == 0) return NULL;

    /* Large allocation via PMM (> SLAB_MAX_SIZE) */
    if (size > SLAB_MAX_SIZE) {
        uint32_t total = size + sizeof(large_hdr_t);
        uint32_t pages = (total + SLAB_PAGE_SIZE - 1) / SLAB_PAGE_SIZE;
        large_hdr_t* hdr = (large_hdr_t*)pmm_alloc_blocks(pages);
        if (!hdr) return NULL;

        hdr->magic = LARGE_MAGIC;
        hdr->pages = pages;
        total_committed += pages * SLAB_PAGE_SIZE;
        return (void*)((uint32_t)hdr + sizeof(large_hdr_t));
    }

    /* Find the smallest cache class that fits this size */
    int idx = 0;
    while (idx < SLAB_CACHE_COUNT - 1 && slab_sizes[idx] < size) {
        idx++;
    }
    return slab_alloc(idx);
}

/**
 * @brief Free kernel memory previously allocated by kmalloc.
 *
 * Uses magic number at the page boundary to determine type:
 * - SLAB_MAGIC: standard slab free
 * - LARGE_MAGIC: PMM-backed large alloc
 * - Neither: silent no-op (defensive, corrupted or invalid pointer)
 *
 * @param ptr Pointer to free, or NULL (no-op).
 */
void kfree(void* ptr) {
    if (!ptr) return;

    slab_t* slab = ptr_to_slab(ptr);
    if (!slab) return;

    if (slab->magic == SLAB_MAGIC) {
        slab_free(slab, ptr);
    } else if (slab->magic == LARGE_MAGIC) {
        large_hdr_t* hdr = (large_hdr_t*)slab;
        total_committed -= hdr->pages * SLAB_PAGE_SIZE;
        pmm_free_blocks(hdr, hdr->pages);
    }
}

/**
 * @brief Initialize the kernel heap slab allocator.
 *
 * Pre-computes object counts for all 8 caches.
 * No pre-allocation — slabs are allocated on first kmalloc.
 */
void init_kheap(void) {
    init_caches();
}

/* Initcall: runs early in boot (before tasking, after PMM) */
static void init_kheap_wrapper(void) {
    init_kheap();
}
early_init(init_kheap_wrapper);

uint32_t kheap_get_used(void) {
    return total_committed;
}

uint32_t kheap_get_free(void) {
    uint32_t free_bytes = 0;
    for (int i = 0; i < SLAB_CACHE_COUNT; i++) {
        slab_t* slab = caches[i].slab_list;
        while (slab) {
            free_bytes += slab->free_count * slab->obj_size;
            slab = slab->next;
        }
    }
    return free_bytes;
}

/**
 * @brief Return kheap_get_used() for backward compatibility.
 * The old "current pointer" concept no longer exists.
 */
uint32_t kheap_get_current(void) {
    return kheap_get_used();
}

uint32_t kheap_get_start(void) {
    return 0;  /* No longer applicable with slab allocator */
}

/**
 * @brief Free completely empty slabs back to PMM.
 *
 * Uses a prev pointer instead of &slab->next to avoid
 * -Waddress-of-packed-member on the packed slab_t struct.
 */
void kheap_reclaim(void) {
    for (int i = 0; i < SLAB_CACHE_COUNT; i++) {
        slab_cache_t* cache = &caches[i];
        slab_t* prev = NULL;
        slab_t* slab = cache->slab_list;

        while (slab) {
            slab_t* next = slab->next;
            if (slab->free_count == slab->obj_count && slab->obj_count > 0) {
                if (prev)
                    prev->next = slab->next;
                else
                    cache->slab_list = slab->next;
                total_committed -= SLAB_PAGE_SIZE;
                pmm_free_blocks(slab, 1);
            } else {
                prev = slab;
            }
            slab = next;
        }
    }
}

void* memcpy(void* dest, const void* src, size_t n) {
    unsigned char* d = (unsigned char*)dest;
    const unsigned char* s = (const unsigned char*)src;
    while(n--) *d++ = *s++;
    return dest;
}

void* __memcpy_chk(void* dest, const void* src, size_t len, size_t destlen) {
    (void)destlen;
    return memcpy(dest, src, len);
}

void* memset(void* s, int c, size_t n) {
    unsigned char* p = (unsigned char*)s;
    while(n--) *p++ = (unsigned char)c;
    return s;
}

size_t strlen(const char* s) {
    size_t len = 0;
    while(*s++) len++;
    return len;
}

int strcmp(const char* s1, const char* s2) {
    while(*s1 && (*s1 == *s2)) { s1++; s2++; }
    return *(const unsigned char*)s1 - *(const unsigned char*)s2;
}

int strncmp(const char* s1, const char* s2, size_t n) {
    while(n--) {
        if(*s1 != *s2) return *(const unsigned char*)s1 - *(const unsigned char*)s2;
        if(*s1 == 0) break;
        s1++; s2++;
    }
    return 0;
}

char* strcpy(char* dest, const char* src) {
    char* d = dest;
    while((*d++ = *src++));
    return dest;
}

char* __strcpy_chk(char* dest, const char* src, size_t destlen) {
    (void)destlen;
    return strcpy(dest, src);
}

char* strcat(char* dest, const char* src) {
    char* d = dest;
    while (*d) d++;
    while ((*d++ = *src++));
    return dest;
}

char* __strcat_chk(char* dest, const char* src, size_t destlen) {
    (void)destlen;
    return strcat(dest, src);
}

char* strstr(const char* haystack, const char* needle) {
    if (!*needle) return (char*)haystack;
    for (; *haystack; haystack++) {
        const char* h = haystack;
        const char* n = needle;
        while (*h && *n && *h == *n) { h++; n++; }
        if (!*n) return (char*)haystack;
    }
    return NULL;
}

char* strchr(const char* s, int c) {
    for (; *s; s++) { if (*s == (char)c) return (char*)s; }
    return NULL;
}

char* strrchr(const char* s, int c) {
    char* last = NULL;
    for (; *s; s++) { if (*s == (char)c) last = (char*)s; }
    return last;
}

char* strncpy(char* dest, const char* src, size_t n) {
    char* d = dest;
    while (n-- && (*d++ = *src++));
    /* Null-pad the remainder (standard strncpy behavior) */
    while (n-- > 0) *d++ = '\0';
    return dest;
}
