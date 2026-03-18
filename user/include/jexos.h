/**
 * @file jexos.h
 * @brief Kernel system call wrappers for user-space.
 *
 * Provides inline assembly wrappers for standard system calls like
 * print, open, read, write, and sbrk.
 */

#ifndef JEXOS_H
#define JEXOS_H

#include <stdint.h>
#include <stddef.h>

/**
 * @brief Print a string to the kernel console.
 */
static inline int sys_print(const char* msg) {
    int ret;
    asm volatile("int $0x80" : "=a"(ret) : "a"(0), "b"(msg));
    return ret;
}

/**
 * @brief Exit the current process.
 */
static inline void sys_exit() {
    asm volatile("int $0x80" : : "a"(1));
    while(1);
}

/**
 * @brief Open a file on disk.
 */
static inline int sys_open(const char* filename, int flags) {
    int ret;
    asm volatile("int $0x80" : "=a"(ret) : "a"(2), "b"(filename), "c"(flags));
    return ret;
}

/**
 * @brief Read data from an open file.
 */
static inline int sys_read(int fd, void* buffer, size_t size) {
    int ret;
    asm volatile("int $0x80" : "=a"(ret) : "a"(3), "b"(fd), "c"(buffer), "d"(size));
    return ret;
}

/**
 * @brief Write data to an open file.
 */
static inline int sys_write(int fd, const void* buffer, size_t size) {
    int ret;
    asm volatile("int $0x80" : "=a"(ret) : "a"(4), "b"(fd), "c"(buffer), "d"(size));
    return ret;
}

/**
 * @brief Close an open file descriptor.
 */
static inline int sys_close(int fd) {
    int ret;
    asm volatile("int $0x80" : "=a"(ret) : "a"(5), "b"(fd));
    return ret;
}

/**
 * @brief Reposition the file offset.
 */
static inline int sys_seek(int fd, int offset, int whence) {
    int ret;
    asm volatile("int $0x80" : "=a"(ret) : "a"(6), "b"(fd), "c"(offset), "d"(whence));
    return ret;
}

/**
 * @brief Request more memory from the kernel.
 */
static inline void* sys_sbrk(intptr_t increment) {
    void* ret;
    asm volatile("int $0x80" : "=a"(ret) : "a"(7), "b"(increment));
    return ret;
}

#endif // JEXOS_H
