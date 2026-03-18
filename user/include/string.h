/**
 * @file string.h
 * @brief String and memory manipulation functions for user-space.
 */

#ifndef STRING_H
#define STRING_H

#include <stddef.h>

void* memcpy(void* dest, const void* src, size_t n);
void* memset(void* s, int c, size_t n);
size_t strlen(const char* s);
int strcmp(const char* s1, const char* s2);
char* strcpy(char* dest, const char* src);

#endif // STRING_H
