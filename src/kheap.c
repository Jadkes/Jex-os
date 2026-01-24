#include "kheap.h"
#include "paging.h"
#include "string.h"
#include <stddef.h>

extern void terminal_writestring(const char* data);

void* kmalloc(size_t size) {
    static uint32_t heap_ptr = 0x1000000;
    uint32_t old_ptr = heap_ptr;
    heap_ptr += size;
    return (void*)old_ptr;
}

void kfree(void *p) { (void)p; }

void init_kheap(uint32_t start_addr) { (void)start_addr; }

void* memcpy(void* dest, const void* src, size_t n) {
    unsigned char* d = (unsigned char*)dest;
    const unsigned char* s = (const unsigned char*)src;
    while(n--) *d++ = *s++;
    return dest;
}

void* __memcpy_chk(void* dest, const void* src, size_t len, size_t destlen) {
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
    return strcpy(dest, src);
}

char* strcat(char* dest, const char* src) {
    char* d = dest;
    while (*d) d++;
    while ((*d++ = *src++));
    return dest;
}

char* __strcat_chk(char* dest, const char* src, size_t destlen) {
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
    if (n) *d = '\0';
    return dest;
}
