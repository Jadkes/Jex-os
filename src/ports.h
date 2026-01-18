#ifndef PORTS_H
#define PORTS_H

#include <stdint.h>

/* Read a byte from a port */
static inline uint8_t inb(uint16_t port)
{
    uint8_t ret;
    asm volatile ( "inb %1, %0"
                   : "=a"(ret)
                   : "Nd"(port) );
    return ret;
}

/* Write a byte to a port */
static inline void outb(uint16_t port, uint8_t val)
{
    asm volatile ( "outb %0, %1" : : "a"(val), "Nd"(port) );
}

/* Wait a very small amount of time (useful for some hardware ops) */
static inline void io_wait(void)
{
    outb(0x80, 0);
}

#endif
