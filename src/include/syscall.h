/**
 * @file syscall.h
 * @brief System Call interface.
 *
 * Defines the syscall numbers and initialization.
 */

#ifndef SYSCALL_H
#define SYSCALL_H

#include "isr.h"

/**
 * @brief Initialize the system call dispatcher (Interrupt 0x80).
 */
void init_syscalls();

/* System call numbers */
#define SYS_PRINT    0
#define SYS_EXIT     1
#define SYS_OPEN     2
#define SYS_READ     3
#define SYS_WRITE    4
#define SYS_CLOSE    5
#define SYS_SEEK     6
#define SYS_SBRK     7
#define SYS_EXECVE   8
#define SYS_FORK     9
#define SYS_WAITPID  10
#define SYS_PRINT_INT 11
#define SYS_PRINT_CHAR 12

/* File access modes */
#define O_RDONLY    0x00
#define O_WRONLY    0x01
#define O_RDWR      0x02
#define O_CREAT     0x40
#define O_APPEND    0x08

#endif // SYSCALL_H
