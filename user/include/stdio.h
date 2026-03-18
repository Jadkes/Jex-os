/**
 * @file stdio.h
 * @brief Standard I/O library for JexOS user-space.
 */

#ifndef STDIO_H
#define STDIO_H

#include <stddef.h>
#include <stdarg.h>

/**
 * @brief Simple file handle.
 */
typedef int FILE;

/**
 * @brief Print a formatted string to the console.
 */
void printf(const char* format, ...);

/**
 * @brief Open a file.
 */
int fopen(const char* filename, const char* mode);

/**
 * @brief Close a file.
 */
void fclose(int fd);

/**
 * @brief Read data from a file.
 */
int fread(void* ptr, size_t size, size_t nmemb, int fd);

/**
 * @brief Write data to a file.
 */
int fwrite(const void* ptr, size_t size, size_t nmemb, int fd);

#endif // STDIO_H
