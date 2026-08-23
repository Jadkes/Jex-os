/**
 * @file ports.h
 * @brief x86 I/O Port communication functions.
 *
 * Provides inline assembly wrappers for 'in' and 'out' instructions.
 */

#ifndef PORTS_H
#define PORTS_H

#include <stdint.h>

/* ── Legacy PC hardware ports ───────────────────────────
 * Shared port/bitmap definitions for the i8254 PIT, PS/2
 * 8042 controller, and the NMI/system-control register.
 * Device-specific bits live in each driver's header.     */

#define POST_CODE_PORT        0x80    /* any write stalls ~1us        */

/* i8254 PIT: three counters behind one command register */
#define PIT_CH0_DATA          0x40
#define PIT_CH2_DATA          0x42
#define PIT_CMD               0x43
#define PIT_BASE_FREQ_HZ      1193180u

/* Command byte layout: [sc][rw][mode][bcd] */
#define PIT_CMD_CH2           0x80
#define PIT_CMD_ACCESS_LOHI   0x30
#define PIT_CMD_MODE_ONESHOT  0x00
#define PIT_CMD_MODE_RATEGEN  0x04
#define PIT_CMD_MODE_SQUARE   0x06

/* Port 0x61 doubles as NMI status / system control:
 * gate for PIT channel 2, speaker output enable, and the
 * readable state of PIT OUT2 used as a countdown stopwatch. */
#define SYSCTL_PORT           0x61
#define SYSCTL_TIMER2_GATE    0x01
#define SYSCTL_SPEAKER_EN     0x02
#define SYSCTL_TIMER2_OUT     0x20

/* PS/2 8042 keyboard controller */
#define KBD_DATA_PORT         0x60
#define I8042_STATUS_PORT     0x64
#define I8042_CMD_RESET_CPU   0xFE

/* Emulator ACPI PM1a control registers (non-standard ports) */
#define QEMU_PM1A_CNT_PORT    0x604
#define BOCHS_PM1A_CNT_PORT   0xB004

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

/**
 * @brief Disable interrupts, returning the previous EFLAGS value.
 *
 * Pair with irq_restore().  Unlike a bare cli/sti pair this is safe in
 * code reachable from both task context (IF=1) and ISR context (IF=0):
 * the restore re-enables only what the caller had enabled.
 */
static inline uint32_t irq_save(void)
{
    uint32_t flags;
    asm volatile ("pushfl ; popl %0 ; cli" : "=r"(flags) :: "memory");
    return flags;
}

/**
 * @brief Restore the interrupt state captured by irq_save().
 */
static inline void irq_restore(uint32_t flags)
{
    if (flags & (1u << 9))   /* EFLAGS.IF */
        asm volatile ("sti" ::: "memory");
}

#endif // PORTS_H
