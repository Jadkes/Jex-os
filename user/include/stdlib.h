/**
 * @file stdlib.h
 * @brief Standard library functions for memory and process control.
 */

#ifndef STDLIB_H
#define STDLIB_H

#include <stddef.h>

/**
 * @brief Allocate memory from the heap.
 */
void* malloc(size_t size);

/**
 * @brief Free allocated memory.
 */
void free(void* ptr);

/**
 * @brief Terminate the process.
 */
void exit(int status);

/**
 * @brief Convert string to integer.
 */
int atoi(const char* str);

#endif // STDLIB_H
