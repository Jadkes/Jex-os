/*
 * ftrace.h - Lightweight function tracer header
 *
 * Purpose: Provide a ring-buffer-based function tracer using GCC's
 *          -finstrument-functions. Records function entry/exit with
 *          kallsyms-resolved symbolic names. Substring-based filtering.
 * Thread-safety: The ring buffer uses a single volatile head index;
 *                intended for UP systems with interrupts disabled during
 *                the fast path.
 */

#ifndef FTRACE_H
#define FTRACE_H
#include <stdint.h>

#define FTRACE_BUF_SIZE 4096
#define FTRACE_ENTRY 1
#define FTRACE_EXIT  2

typedef struct {
    uint32_t func;     /* called function address */
    uint32_t caller;   /* call site address */
    uint32_t type;     /* FTRACE_ENTRY or FTRACE_EXIT */
} ftrace_record_t;

void ftrace_enable(void);
void ftrace_disable(void);
int  ftrace_is_enabled(void);

/* Filter management -- substring match on function name */
void ftrace_add_filter(const char* substr);
void ftrace_clear_filters(void);

/* Dump ring buffer to serial/terminal/klog */
void ftrace_dump(void);

/* Called by instrumented code (gcc -finstrument-functions) */
void __cyg_profile_func_enter(void* func, void* call_site);
void __cyg_profile_func_exit(void* func, void* call_site);

#endif /* FTRACE_H */
