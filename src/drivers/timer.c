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
#include "apic.h"
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

    /*
     * Preemption moves to the per-CPU LAPIC timers once they run: they
     * fire on every core, while IRQ0 only ever reaches the BSP through
     * the I/O APIC routing.  The PIT stays up as the wall-clock source.
     */
    if (!lapic_timer_armed()) {
        task_t* t = cur_task();
        if (t)
            t->cpu_ticks++;
        task_switch();
    }

    (void)regs;
}

/**
 * @brief LAPIC timer interrupt handler (vector 48).
 *
 * Runs on whichever CPU's timer expired and preempts that CPU only.
 * system_ticks stays PIT-owned so wall-clock time is counted once.
 *
 * @param regs State of CPU registers.
 */
void lapic_timer_tick(registers_t *regs)
{
    task_t* t = cur_task();
    if (t)
        t->cpu_ticks++;
    task_switch();

    (void)regs;
}

/**
 * @brief Arm the LAPIC timer on the calling CPU and register its handler.
 *
 * Registration happens once; arming is per-CPU and safe to call from
 * every core as it comes online.
 */
void init_lapic_timer(void)
{
    if (!using_apic) return;   /* PIC-only box: PIT keeps preemption */


    static int registered = 0;
    if (!registered) {
        registered = 1;
        register_interrupt_handler(16, lapic_timer_tick);  /* vector 48 */
    }

    /* BSP calibrates on first call; APs arrive after smp_timers_go()
     * with the interval already set and skip straight to arming. */
    if (!lapic_timer_interval)
        lapic_timer_calibrate();

    lapic_timer_arm();
}

/**
 * @brief Initialize the PIT at a target frequency.
 * 
 * @param frequency Frequency in Hertz (e.g., 100).
 */
void init_timer(uint32_t frequency)
{
    if (frequency == 0) frequency = TICKS_PER_SEC;

    /* Register IRQ0 handler.  With the 8259 masked and the IO/APIC owning
     * delivery, QEMU's PIIX3 routes the PIT output through the legacy
     * cascade line, which lands on vector 34 (slot 2) instead of 32 — so
     * register the callback on both slots. */
    register_interrupt_handler(0, timer_callback);
    register_interrupt_handler(2, timer_callback);

    uint32_t divisor = PIT_BASE_FREQ_HZ / frequency;

    /* Channel 0, lo/hi access, rate generator.  Mode 3's square wave
     * toggles twice per period and QEMU's cascade-line path delivers an
     * extra edge for it, doubling the tick rate; mode 2 pulses OUT low
     * once per period, giving exactly one rising edge per tick. */
    outb(PIT_CMD, PIT_CMD_ACCESS_LOHI | PIT_CMD_MODE_RATEGEN);
    outb(PIT_CH0_DATA, (uint8_t)(divisor & 0xFF));
    outb(PIT_CH0_DATA, (uint8_t)(divisor >> 8));
}

/**
 * @brief Wait for a specific duration in milliseconds.
 *
 * @param ms Milliseconds to wait.
 */
void sleep(uint32_t ms)
{
    uint32_t start_ticks = system_ticks;
    /* Ceiling division ensures a non-zero wait for small requests */
    uint32_t ticks_to_wait = (ms + MS_PER_TICK - 1) / MS_PER_TICK;
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
    /* 50 ms window: 1.193182 MHz * 0.05 s = 59659 counts, within 16 bits */
    const uint32_t stopwatch_counts = PIT_BASE_FREQ_HZ / (1000 / SELFTEST_WINDOW_MS);
    uint8_t port61 = inb(SYSCTL_PORT);
    /* Gate channel 2 on, keep the speaker output off. */
    outb(SYSCTL_PORT, (uint8_t)((port61 & ~SYSCTL_SPEAKER_EN) | SYSCTL_TIMER2_GATE));

    outb(PIT_CMD, PIT_CMD_CH2 | PIT_CMD_ACCESS_LOHI | PIT_CMD_MODE_ONESHOT);
    outb(PIT_CH2_DATA, (uint8_t)(stopwatch_counts & 0xFF));
    outb(PIT_CH2_DATA, (uint8_t)(stopwatch_counts >> 8));

    uint32_t start_ticks = system_ticks;
    while (!(inb(SYSCTL_PORT) & SYSCTL_TIMER2_OUT))
        __asm__ volatile("pause");
    uint32_t elapsed = system_ticks - start_ticks;

    outb(SYSCTL_PORT, port61);      /* restore speaker gating */

    pr_info("clock self-test: %u ticks in a %u ms window (expected %u)\n",
            elapsed, SELFTEST_WINDOW_MS, SELFTEST_WINDOW_MS / MS_PER_TICK);
}
