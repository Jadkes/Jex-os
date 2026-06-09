#ifndef PANIC_H
#define PANIC_H

#include <stdint.h>
#include "isr.h"

/* Page fault error code bits */
#define PF_ERR_PRESENT  0x01
#define PF_ERR_WRITE    0x02
#define PF_ERR_USER     0x04
#define PF_ERR_RSVD     0x08
#define PF_ERR_INSTR    0x10

void panic_handler(registers_t* regs);
void panic_assert(const char* msg, const char* file, int line);
void format_hex(uint32_t val, char* out);
int unwind_stack(uint32_t ebp, uint32_t* eip_out, int max_frames);
void decode_page_fault_err(uint32_t err_code, char* buf, int buf_len);

#endif /* PANIC_H */
