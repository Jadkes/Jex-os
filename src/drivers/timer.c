/**
 * @file timer.c
 * @brief Programmable Interval Timer (PIT) implementation.
 *
 * Configures the PIT to generate periodic interrupts (IRQ0) for system timing
 * and preemptive multitasking.
 */

#include "timer.h"
#include "isr.h"
#include "irq.h"
#include "ports.h"
#include "task.h"
#include "kernel/printk.h"
/**
 * @brief Global system tick counter.
 * Marked volatile because it is modified within an interrupt handler.
 */
volatile uint32_t system_ticks = 0;

/**
 * @brief Timer Interrupt Handler (IRQ0).
 * Called every time the PIT finishes counting down.
 * 
 * @param regs State of CPU registers.
 */
void timer_callback(registers_t *regs)
{
    system_ticks++;

    /* Track per-task CPU usage for the top command */
    task_t* t = cur_task();
    if (t)
        t->cpu_ticks++;

    /* Trigger the scheduler to possibly switch tasks */
    task_switch();

    (void)regs;
}

/**
 * @brief Initialize the PIT at a target frequency.
 * 
 * @param frequency Frequency in Hertz (e.g., 100).
 */
void init_timer(uint32_t frequency)
{
    if (frequency == 0) frequency = 100;  /* Guard against div-by-zero */

    /* Register IRQ0 handler.  With the 8259 masked and the IO/APIC owning
     * delivery, QEMU's PIIX3 routes the PIT output through the legacy
     * cascade line, which lands on vector 34 (slot 2) instead of 32 — so
     * register the callback on both slots. */
    register_interrupt_handler(0, timer_callback);
    register_interrupt_handler(2, timer_callback);

    /* PIT base frequency is 1.193182 MHz */
    uint32_t divisor = 1193180 / frequency;

    /* Send control byte: Channel 0, access lo/hi byte, rate generator
     * (mode 2), 16-bit binary.  Mode 3's square wave toggles twice per
     * period and QEMU's cascade-line path delivers an extra edge for it,
     * doubling the tick rate; mode 2 pulses OUT low once per period,
     * giving exactly one rising edge per tick. */
    outb(0x43, 0x34);

    /* Send divisor bytes */
    uint8_t l = (uint8_t)(divisor & 0xFF);
    uint8_t h = (uint8_t)((divisor >> 8) & 0xFF);
    outb(0x40, l);
    outb(0x40, h);
}

/**
 * @brief Wait for a specific duration in milliseconds.
 * 
 * @param ms Milliseconds to wait.
 */
void sleep(uint32_t ms)
{
    uint32_t start_ticks = system_ticks;
    /* Assumes 100Hz frequency (10ms per tick). Ceiling division ensures
     * non-zero wait for small requests (e.g., 5ms → 1 tick). */
    uint32_t ticks_to_wait = (ms + 9) / 10;
    if (ticks_to_wait == 0) ticks_to_wait = 1;

    /* Ticks only advance while interrupts flow, so IF must be ON during
     * the wait — a bare hlt with IF=0 sleeps forever because no tick can
     * ever wake the CPU.  Capture the caller's state, enable, wait, then
     * restore, so sleep never leaks an IF change into its caller. */
    uint32_t eflags = irq_save();
    __asm__ volatile("sti");

    while(system_ticks < start_ticks + ticks_to_wait)
    {
        /* Halt the CPU until the next interrupt to save power */
        __asm__ volatile("hlt");
    }

    irq_restore(eflags);
}

/**
 * @brief Get the current system uptime in ticks.
 */
uint32_t get_ticks()
{
    return system_ticks;
}

/**
 * @brief Verify the delivered tick rate against an independent stopwatch.
 *
 * Runs channel 2 as a 50 ms one-shot (1.193182 MHz * 0.05 = 59659 counts)
 * and counts how many timer callbacks arrive before OUT2 rises.  This
 * measures what sleep() actually experiences — if the clock is misrouted
 * or double-delivered it shows up in this one boot line instead of as
 * subtly-fast music.  Must be called with interrupts enabled; exits
 * within 50 ms even if no ticks ever arrive (the PIT runs on its own).
 */
void timer_selftest(void)
{
    uint8_t port61 = inb(0x61);
    /* Gate channel 2 on, keep the speaker output (bit 1) off. */
    outb(0x61, (uint8_t)((port61 & ~0x02) | 0x01));

    outb(0x43, 0xB0);               /* ch2, lo/hi access, mode 0 */
    outb(0x42, 59659 & 0xFF);
    outb(0x42, (59659 >> 8) & 0xFF);

    uint32_t start_ticks = system_ticks;
    while (!(inb(0x61) & 0x20))     /* OUT2 high at terminal count */
        __asm__ volatile("pause");
    uint32_t elapsed = system_ticks - start_ticks;

    outb(0x61, port61);             /* restore speaker gating */

    pr_info("clock self-test: %u ticks in a 50 ms window (expected 5)\n",
            elapsed);
}
