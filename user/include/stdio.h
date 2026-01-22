#ifndef STDIO_H
#define STDIO_H

#include <stddef.h>
#include <stdarg.h>

typedef int FILE; // Simple FD-based FILE for now

void printf(const char* format, ...);
int fopen(const char* filename, const char* mode);
void fclose(int fd);
int fread(void* ptr, size_t size, size_t nmemb, int fd);
int fwrite(const void* ptr, size_t size, size_t nmemb, int fd);

#endif
