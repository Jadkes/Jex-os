/**
 * @file types.h
 * @brief Standard architecture-independent type definitions.
 */

#ifndef SYS_TYPES_H
#define SYS_TYPES_H

#include <stddef.h>

/* Fixed-width integer types */
typedef signed char int8_t;
typedef unsigned char uint8_t;
typedef signed short int16_t;
typedef unsigned short uint16_t;
typedef signed int int32_t;
typedef unsigned int uint32_t;
typedef signed long long int64_t;
typedef unsigned long long uint64_t;

/* POSIX types */
typedef int32_t ssize_t;
typedef int32_t off_t;
typedef int32_t pid_t;

#endif // SYS_TYPES_H
