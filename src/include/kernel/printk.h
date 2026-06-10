/*
 * printk.h - pr_* logging macro system
 *
 * Purpose: Provide Linux-compatible pr_emerg through pr_debug macros with
 *          runtime console_loglevel filtering and klog integration.
 * Thread-safety: _printk uses a stack buffer; in_isr flag suppresses
 *                slow terminal PIO during ISR context.
 */

#ifndef PRINTK_H
#define PRINTK_H

#include <stdarg.h>

/* Log levels (Linux-compatible numeric constants) */
#define LOG_EMERG   0
#define LOG_ALERT   1
#define LOG_CRIT    2
#define LOG_ERR     3
#define LOG_WARNING 4
#define LOG_NOTICE  5
#define LOG_INFO    6
#define LOG_DEBUG   7

/*
 * Subsystem format tag -- define pr_fmt before including this header
 * to prefix every pr_* message with a subsystem label:
 *
 *   #define pr_fmt(fmt) "[NET] " fmt
 *   #include <kernel/printk.h>
 */
#ifndef pr_fmt
#define pr_fmt(fmt) fmt
#endif

/* Runtime console log level -- messages above this are suppressed */
extern int console_loglevel;

/*
 * _printk - Core printk function
 * @level:  Log severity (LOG_EMERG .. LOG_DEBUG)
 * @fmt:    Format string
 * @...:    Variable arguments
 *
 * Writes formatted output to serial (always), terminal (unless in ISR),
 * and the kernel ring buffer (klog).
 */
void _printk(int level, const char* fmt, ...);

/* pr_* convenience macros */
#define pr_emerg(fmt, ...)   _printk(LOG_EMERG,   pr_fmt(fmt), ##__VA_ARGS__)
#define pr_alert(fmt, ...)   _printk(LOG_ALERT,   pr_fmt(fmt), ##__VA_ARGS__)
#define pr_crit(fmt, ...)    _printk(LOG_CRIT,    pr_fmt(fmt), ##__VA_ARGS__)
#define pr_err(fmt, ...)     _printk(LOG_ERR,     pr_fmt(fmt), ##__VA_ARGS__)
#define pr_warning(fmt, ...) _printk(LOG_WARNING, pr_fmt(fmt), ##__VA_ARGS__)
#define pr_warn              pr_warning
#define pr_notice(fmt, ...)  _printk(LOG_NOTICE,  pr_fmt(fmt), ##__VA_ARGS__)
#define pr_info(fmt, ...)    _printk(LOG_INFO,    pr_fmt(fmt), ##__VA_ARGS__)
#define pr_debug(fmt, ...)   _printk(LOG_DEBUG,   pr_fmt(fmt), ##__VA_ARGS__)
#define pr_cont(fmt, ...)    _printk(LOG_INFO,    fmt, ##__VA_ARGS__)

/* ISR context flag -- set by IRQ entry, cleared on exit */
extern volatile int in_isr;

#endif /* PRINTK_H */
