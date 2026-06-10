#ifndef VSNPRINTF_H
#define VSNPRINTF_H

#include <stddef.h>
#include <stdarg.h>

/*
 * vsnprintf - Format a string with a va_list
 * @buf:  Output buffer
 * @size: Size of output buffer
 * @fmt:  Format string
 * @args: Variable argument list
 *
 * Supports: %d, %i, %u, %x, %X, %p, %s, %c, %%
 * Flags:   Zero-padding ('0'), width, left-justify ('-')
 * No floating-point or 64-bit division.
 *
 * Returns the number of characters that would be written
 * (excluding the null terminator) if size were unlimited.
 */
int vsnprintf(char* buf, size_t size, const char* fmt, va_list args);

/*
 * snprintf - Format a string
 * @buf:  Output buffer
 * @size: Size of output buffer
 * @fmt:  Format string
 *
 * Returns the number of characters that would be written
 * (excluding the null terminator) if size were unlimited.
 */
int snprintf(char* buf, size_t size, const char* fmt, ...);

#endif /* VSNPRINTF_H */
