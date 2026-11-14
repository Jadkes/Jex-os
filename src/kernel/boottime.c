/**
 * @file boottime.c
 * @brief Boot-time measurement for the boottime flavor.
 *
 * Purpose: report the time from the first kernel instruction to the
 * moment the shell prompt is usable, then power off so an external
 * timer can capture the whole GRUB + kernel boot.
 *
 * Design: the TSC is readable before the PIT exists, so it anchors the
 * measurement at kernel entry.  Once interrupts are live the rate is
 * calibrated against the 100 Hz PIT; both the calibration window and
 * its alignment delay are subtracted from the reported total, so the
 * measurement itself does not inflate the number.  The report goes to
 * serial and the machine powers off immediately afterwards.
 *
 * Thread-safety: all hooks run once, on the BSP, during single-threaded
 * boot.
 */

#ifdef JEXOS_BOOTTIME

#define pr_fmt(fmt) "[BOOTTIME] " fmt

#include "boottime.h"
#include "kernel/printk.h"
#include "timer.h"
#include "power.h"
#include <stdint.h>

/* 20 PIT ticks = 200 ms.  A shorter window calibrates faster but the
 * tick quantisation error grows; this is subtracted from the total. */
#define CALIBRATION_TICKS 20

static uint64_t boot_start_tsc;
static uint64_t calibration_tsc;
static uint32_t tsc_per_ms;
static int tsc_available;

static uint64_t read_tsc(void)
{
    uint32_t low, high;

    __asm__ volatile("rdtsc" : "=a"(low), "=d"(high));
    return ((uint64_t)high << 32) | low;
}

static int cpu_has_tsc(void)
{
    uint32_t eax = 1;
    uint32_t ebx, ecx, edx;

    __asm__ volatile("cpuid"
                     : "+a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx));
    return (edx >> 4) & 1u;
}

/*
 * div_u64_u32 - divide a 64-bit TSC delta by a 32-bit divisor.
 *
 * The kernel links no libgcc, so __udivdi3 does not exist.  divl takes
 * edx:eax and is safe here because the quotient (milliseconds) always
 * fits in 32 bits; anything larger is clamped instead of faulting.
 */
static uint32_t div_u64_u32(uint64_t value, uint32_t divisor)
{
    uint32_t low = (uint32_t)value;
    uint32_t high = (uint32_t)(value >> 32);
    uint32_t quotient;
    uint32_t remainder;

    if (divisor == 0 || high >= divisor)
        return UINT32_MAX;
    /* eax:edx is the dividend, %4 is the divisor (ecx).  The index
     * matters: %2 is the low half and %3 the high half, so dividing by
     * %3 would use the TSC high word instead. */
    __asm__ volatile("divl %4"
                     : "=a"(quotient), "=d"(remainder)
                     : "a"(low), "d"(high), "c"(divisor)
                     : "cc");
    (void)remainder;
    return quotient;
}

void boottime_start(void)
{
    tsc_available = cpu_has_tsc();
    if (tsc_available)
        boot_start_tsc = read_tsc();
}

void boottime_calibrate(void)
{
    uint64_t calib_start;
    uint64_t window_start;
    uint64_t window_end;
    uint64_t calib_end;
    uint32_t align_tick;
    uint32_t start_tick;
    uint32_t elapsed_ticks;

    if (!tsc_available)
        return;

    calib_start = read_tsc();

    /* Start on a tick edge so the window is a whole number of ticks. */
    align_tick = get_ticks();
    while (get_ticks() == align_tick) {
    }

    start_tick = get_ticks();
    window_start = read_tsc();
    while ((uint32_t)(get_ticks() - start_tick) < CALIBRATION_TICKS) {
    }
    window_end = read_tsc();
    elapsed_ticks = get_ticks() - start_tick;
    calib_end = read_tsc();

    calibration_tsc = calib_end - calib_start;
    if (elapsed_ticks == 0)
        return;

    /* 200 ms at any sane clock is well under 2^32 TSC ticks; the cast
     * keeps the division 32-bit so no libgcc helper is needed. */
    tsc_per_ms = (uint32_t)(window_end - window_start) /
                 (elapsed_ticks * MS_PER_TICK);
}

void boottime_shell_ready(void)
{
    uint64_t elapsed_tsc;
    uint32_t total_ms;

    if (!tsc_available || tsc_per_ms == 0) {
        pr_serial("shell ready (no TSC; boot time unavailable)\n");
        shutdown();
        return;
    }

    elapsed_tsc = (read_tsc() - boot_start_tsc) - calibration_tsc;
    total_ms = div_u64_u32(elapsed_tsc, tsc_per_ms);
    pr_serial("BOOTTIME: %u ms (kernel entry -> shell ready)\n", total_ms);
    shutdown();
}

#endif /* JEXOS_BOOTTIME */
