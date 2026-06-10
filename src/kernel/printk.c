/*
 * printk.c - pr_* logging implementation
 *
 * Purpose: Provides the _printk() backend that formats messages via
 *          vsnprintf and distributes them to serial, terminal, and klog.
 * Design:  All output goes to serial unconditionally. Terminal output is
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
 * ISR context flag -- set to 1 by irq_handler before dispatching,
 * cleared on return. Used to skip slow terminal PIO during interrupts.
 */
volatile int in_isr = 0;

void _printk(int level, const char* fmt, ...)
{
    /* Level filtering: suppress messages above the threshold */
    if (level > console_loglevel)
        return;

    char buf[256];
    va_list args;

    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    /* Serial output -- always safe, even in ISR context */
    log_serial(buf);

    /*
     * Terminal output -- skip if we are in an ISR.
     * VGA text-mode PIO is too slow and may interact badly with
     * reentrant interrupt handling.
     */
    if (!in_isr)
        terminal_writestring(buf);

    /* Store in the kernel ring buffer for dmesg retrieval */
    klog_write(level, buf);
}
