#include "kheap.h"

uint32_t placement_address;

void init_kheap(uint32_t start_addr) {
    placement_address = start_addr;
}

void *kmalloc(size_t size) {
    if (placement_address & 0xFFF) {
        placement_address &= 0xFFFFF000;
        placement_address += 0x1000;
    }
    
    void *tmp = (void *)placement_address;
    placement_address += size;
    return tmp;
}

void kfree(void *p) {
    (void)p;
}

void* memcpy(void* dest, const void* src, size_t n) {
    unsigned char* d = (unsigned char*)dest;
    const unsigned char* s = (const unsigned char*)src;
    for (size_t i = 0; i < n; i++) {
        d[i] = s[i];
    }
    return dest;
}

void* __memcpy_chk(void* dest, const void* src, size_t len, size_t destlen) {
    // In a real implementation, we would check if len > destlen and panic/abort.
    // For now, we trust the caller (GCC) or just ignore the check for the kernel build.
    return memcpy(dest, src, len);
}

void* memset(void* s, int c, size_t n) {
    unsigned char* p = (unsigned char*)s;
    for (size_t i = 0; i < n; i++) {
        p[i] = (unsigned char)c;
    }
    return s;
}

char* strcpy(char* dest, const char* src) {
    char* d = dest;
    while((*d++ = *src++));
    return dest;
}

char* strstr(const char* haystack, const char* needle) {
    if (!*needle) return (char*)haystack;
    
    for (; *haystack; haystack++) {
        const char* h = haystack;
        const char* n = needle;
        
        while (*h && *n && *h == *n) {
            h++;
            n++;
        }
        
        if (!*n) return (char*)haystack;
    }
    
    return NULL;
}

char* strchr(const char* s, int c) {
    for (; *s; s++) {
        if (*s == (char)c) return (char*)s;
    }
    return NULL;
}

char* strncpy(char* dest, const char* src, size_t n) {
    char* d = dest;
    while (n-- && (*d++ = *src++));
    if (n) *d = '\0';
    return dest;
}