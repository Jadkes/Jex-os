/**
 * @file ports.h
 * @brief x86 I/O Port communication functions.
 *
 * Provides inline assembly wrappers for 'in' and 'out' instructions.
 */

#ifndef PORTS_H
#define PORTS_H

#include <stdint.h>

/**
 * @brief Read a byte from an I/O port.
 */
static inline uint8_t inb(uint16_t port)
{
    uint8_t ret;
    asm volatile ( "inb %1, %0"
                   : "=a"(ret)
                   : "Nd"(port) );
    return ret;
}

/**
 * @brief Write a byte to an I/O port.
 */
static inline void outb(uint16_t port, uint8_t val)
{
    asm volatile ( "outb %0, %1" : : "a"(val), "Nd"(port) );
}

/**
 * @brief Read a 16-bit word from an I/O port.
 */
static inline uint16_t inw(uint16_t port)
{
    uint16_t ret;
    asm volatile ( "inw %1, %0"
                   : "=a"(ret)
                   : "Nd"(port) );
    return ret;
}

/**
 * @brief Write a 16-bit word to an I/O port.
 */
static inline void outw(uint16_t port, uint16_t val)
{
    asm volatile ( "outw %0, %1" : : "a"(val), "Nd"(port) );
}

/**
 * @brief Read a 32-bit dword from an I/O port.
 */
static inline uint32_t inl(uint16_t port)
{
    uint32_t ret;
    asm volatile ( "inl %1, %0"
                   : "=a"(ret)
                   : "Nd"(port) );
    return ret;
}

/**
 * @brief Write a 32-bit dword to an I/O port.
 */
static inline void outl(uint16_t port, uint32_t val)
{
    asm volatile ( "outl %0, %1" : : "a"(val), "Nd"(port) );
}

/**
 * @brief Read a sequence of words from a port into memory.
 */
static inline void insw(uint16_t port, void* addr, uint32_t count)
{
    asm volatile ("cld; rep insw" : "+D"(addr), "+c"(count) : "d"(port) : "memory");
}

/**
 * @brief Write a sequence of words from memory to a port.
 */
static inline void outsw(uint16_t port, const void* addr, uint32_t count)
{
    asm volatile ("cld; rep outsw" : "+S"(addr), "+c"(count) : "d"(port));
}

/**
 * @brief Wait a very small amount of time (for hardware stabilization).
 */
static inline void io_wait(void)
{
    outb(0x80, 0);
}

#endif // PORTS_H
