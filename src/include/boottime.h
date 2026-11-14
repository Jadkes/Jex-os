/**
 * @file boottime.h
 * @brief Boot-time measurement hooks (JEXOS_BOOTTIME builds only).
 *
 * In normal builds every hook compiles to nothing, so callers do not
 * need their own #ifdef.
 */

#ifndef BOOTTIME_H
#define BOOTTIME_H

#ifdef JEXOS_BOOTTIME

/** boottime_start - timestamp the first kernel instruction. */
void boottime_start(void);

/** boottime_calibrate - measure the TSC rate against the PIT. */
void boottime_calibrate(void);

/** boottime_shell_ready - report the total and power the machine off. */
void boottime_shell_ready(void);

#else

static inline void boottime_start(void) {}
static inline void boottime_calibrate(void) {}
static inline void boottime_shell_ready(void) {}

#endif

#endif /* BOOTTIME_H */
