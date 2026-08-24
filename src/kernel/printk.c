/*
 * printk.c - pr_* logging implementation
 *
 * Provides the _printk() backend that formats messages via
 *          vsnprintf and distributes them to serial, terminal, and klog.
 * All output goes to serial unconditionally. Terminal output is
 *          skipped during ISR context to avoid slow PIO on the critical path.
 *          klog stores messages with their log level for dmesg retrieval.
 */

#include "kernel/printk.h"
#include "kernel/vsnprintf.h"
#include "serial.h"
#include "terminal.h"
#include "klog.h"

#ifndef DEFAULT_CONSOLE_LOGLEVEL
#define DEFAULT_CONSOLE_LOGLEVEL 6   /* LOG_INFO and above by default */
#endif

/* Runtime filter: only messages at or below this level reach the console */
int console_loglevel = DEFAULT_CONSOLE_LOGLEVEL;

/*
 * Serial-debug channel: when set, LOG_DEBUG messages bypass the
 * console_loglevel filter for SERIAL and KLOG but never touch the
 * VGA terminal.  Lets a subsystem (e.g. ping -v) flood the serial
 * console with trace while the shell stays readable.
 */
#ifdef JEXOS_DEBUG
int serial_debug = 1;
#else
int serial_debug = 0;
#endif

/*
 * ISR nesting depth -- incremented by irq_handler before dispatching,
 * decremented on return.  Used to suppress terminal output while any
 * hardware interrupt is in flight: VGA text-mode PIO is slow and would
 * interleave with a preempted task's half-finished screen update.
 *
 * This MUST be a counter, not a flag: handlers nest (e.g. a timer tick
 * arriving inside the NIC ISR's interrupt-enabled window), and an inner
 * exit clearing a boolean would un-suppress the outer frame too early.
 */
volatile uint32_t in_isr = 0;

void _printk(int level, const char* fmt, ...)
{
    int to_terminal = (level <= console_loglevel);

    /* Suppress unless: console-level message, or serial-only debug */
    if (!to_terminal && !(serial_debug && level == LOG_DEBUG))
        return;

    char buf[256];
    va_list args;

    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    /* Serial output -- always safe, even in ISR context */
    log_serial(buf);

    /*
     * Terminal output -- skip if we are in an ISR or if this is an
     * serial-debug-only message.  VGA text-mode PIO is too slow and
     * may interact badly with reentrant interrupt handling.
     */
    if (to_terminal && !in_isr)
        terminal_writestring(buf);

    /* Store in the kernel ring buffer for dmesg retrieval */
    klog_write(level, buf);
}

void _printk_serial_only(int level, const char* fmt, ...)
{
    char buf[256];
    va_list args;

    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    log_serial(buf);
    klog_write(level, buf);
}
