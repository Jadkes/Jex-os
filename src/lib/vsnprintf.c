/*
 * vsnprintf.c - Minimal kernel vsnprintf/snprintf
 *
 * Purpose: Freestanding printf-family for kernel use.
 * Design: Hand-rolled per-specifier helpers; no floating point, no 64-bit divide.
 * Thread-safety: No global state -- all output goes to a caller-provided buffer.
 */

#include "kernel/vsnprintf.h"
#include <stdint.h>

/*
 * print_dec - Format a signed or unsigned decimal integer
 * @p:     Current position pointer (in-out)
 * @end:   End of buffer (one past last writable byte)
 * @val:   Value to format
 * @sign:  Non-zero if signed (respects negative)
 * @width: Minimum field width
 * @zero:  Non-zero to zero-pad instead of space-pad
 */
static void print_dec(char** p, char* end, uint32_t val, int sign,
                       int width, int zero)
{
    char tmp[12];
    int neg = 0, len = 0;

    if (sign && (int32_t)val < 0) {
        neg = 1;
        val = (uint32_t)-(int32_t)val;
    }

    do {
        tmp[len++] = '0' + (val % 10);
        val /= 10;
    } while (val > 0);

    if (neg)
        tmp[len++] = '-';

    while (len < width && zero)
        tmp[len++] = '0';
    while (len < width)
        tmp[len++] = ' ';

    for (int i = len - 1; i >= 0 && *p < end; i--)
        *(*p)++ = tmp[i];
}

/*
 * print_hex - Format an unsigned hexadecimal integer
 * @p:     Current position pointer (in-out)
 * @end:   End of buffer (one past last writable byte)
 * @val:   Value to format
 * @upper: Non-zero for uppercase hex digits
 * @width: Minimum field width (0 = print all 8 hex digits)
 * @zero:  Non-zero to zero-pad
 */
static void print_hex(char** p, char* end, uint32_t val, int upper,
                       int width, int zero)
{
    const char* digits = upper ?
        "0123456789ABCDEF" : "0123456789abcdef";
    char tmp[10];
    int len = 0;

    if (width == 0) {
        /* No width given: print all 8 hex digits (no 0x prefix) */
        for (int i = 7; i >= 0; i--)
            tmp[len++] = digits[(val >> (i * 4)) & 0xF];
    } else {
        do {
            tmp[len++] = digits[val & 0xF];
            val >>= 4;
        } while (val > 0);
    }

    while (len < width && zero)
        tmp[len++] = '0';
    while (len < width)
        tmp[len++] = ' ';

    for (int i = len - 1; i >= 0 && *p < end; i--)
        *(*p)++ = tmp[i];
}

/*
 * vsnprintf - Format a string with a va_list
 *
 * See vsnprintf.h for full documentation.
 */
int vsnprintf(char* buf, size_t size, const char* fmt, va_list args)
{
    char* p = buf;
    char* end = buf + size - 1;

    for (; *fmt && p < end; fmt++) {
        if (*fmt != '%') {
            *p++ = *fmt;
            continue;
        }

        fmt++;  /* skip '%' */

        /* Parse flags and width */
        int zero = 0;
        int width = 0;
        int left = 0;

        while (*fmt == '-' || *fmt == '0') {
            if (*fmt == '-')
                left = 1;
            if (*fmt == '0')
                zero = 1;
            fmt++;
        }
        while (*fmt >= '0' && *fmt <= '9')
            width = width * 10 + (*fmt++ - '0');
        if (left)
            zero = 0;

        /* Parse length modifier ('l' accepted but ignored on 32-bit) */
        int is_long = 0;
        if (*fmt == 'l') {
            is_long = 1;
            fmt++;
        }
        (void)is_long;  /* %ld == %d on 32-bit */

        switch (*fmt) {
            case 'd':
            case 'i':
                print_dec(&p, end, va_arg(args, int), 1, width, zero);
                break;
            case 'u':
                print_dec(&p, end, va_arg(args, unsigned int), 0, width, zero);
                break;
            case 'x':
                print_hex(&p, end, va_arg(args, unsigned int), 0, width, zero);
                break;
            case 'X':
                print_hex(&p, end, va_arg(args, unsigned int), 1, width, zero);
                break;
            case 'p': {
                uint32_t v = (uint32_t)va_arg(args, void*);
                if (p < end) *p++ = '0';
                if (p < end) *p++ = 'x';
                print_hex(&p, end, v, 0, 8, 1);
                break;
            }
            case 's': {
                const char* s = va_arg(args, const char*);
                if (!s)
                    s = "(null)";
                while (*s && p < end)
                    *p++ = *s++;
                break;
            }
            case 'c':
                if (p < end)
                    *p++ = (char)va_arg(args, int);
                break;
            case '%':
                if (p < end)
                    *p++ = '%';
                break;
            default:
                /* Unknown specifier: emit '%' then the character */
                if (p < end) *p++ = '%';
                if (p < end) *p++ = *fmt;
                break;
        }
    }

    *p = '\0';
    return (int)(p - buf);
}

/*
 * snprintf - Format a string (varargs wrapper around vsnprintf)
 *
 * See vsnprintf.h for full documentation.
 */
int snprintf(char* buf, size_t size, const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    int r = vsnprintf(buf, size, fmt, args);
    va_end(args);
    return r;
}
