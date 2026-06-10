#ifndef BACKTRACE_H
#define BACKTRACE_H
#include <stdint.h>

void format_hex(uint32_t val, char* out);
int unwind_stack(uint32_t ebp, uint32_t* eip_out, int max_frames);
void dump_stack(void);
void dump_stack_serial(void);
#endif
