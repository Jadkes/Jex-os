#ifndef BACKTRACE_H
#define BACKTRACE_H

#include <stdint.h>

#define MAX_BACKTRACE_DEPTH 16

/*
 * format_hex - Format a 32-bit value as "0x12345678"
 * @val:   Value to format
 * @out:   Output buffer (must be >= 11 bytes)
 */
void format_hex(uint32_t val, char* out);

/*
 * unwind_stack - Walk the EBP-linked call stack
 * @ebp:       Current frame pointer
 * @eip_out:   Array to store return addresses (must hold >= max_frames)
 * @max_frames: Maximum frames to capture
 *
 * Returns the number of frames captured.
 */
int unwind_stack(uint32_t ebp, uint32_t* eip_out, int max_frames);

/*
 * dump_stack - Print stack trace to serial and terminal
 */
void dump_stack(void);

/*
 * dump_stack_serial - Print stack trace to serial only
 */
void dump_stack_serial(void);

#endif /* BACKTRACE_H */
