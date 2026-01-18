/* Defines the interface for all interrupt service routines. */
#ifndef ISR_H
#define ISR_H

#include <stdint.h>

/* Registers struct passed from assembly to C */
typedef struct registers
{
    uint32_t ds;                  // Data segment selector
    uint32_t edi, esi, ebp, esp, ebx, edx, ecx, eax; // Pushed by pusha.
    uint32_t int_no, err_code;    // Interrupt number and error code (if applicable)
    uint32_t eip, cs, eflags, useresp, ss; // Pushed by the processor automatically.
} registers_t;

void isr_install();
void isr_handler(registers_t regs);

#endif
