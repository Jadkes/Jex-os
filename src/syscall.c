#include "syscall.h"
#include "idt.h"
#include <stdint.h>

extern void terminal_writestring(const char* data);
extern void isr128();

void syscall_handler(registers_t *regs)
{
    /* The syscall number is in EAX */
    if (regs->eax == 0) /* print syscall */
    {
        terminal_writestring((const char*)regs->ebx);
    }
}

void init_syscalls()
{
    /* We'll use interrupt 0x80 for syscalls. 
       0xEE = Present, Ring 3, Interrupt Gate */
    idt_set_gate(0x80, (uint32_t)isr128, 0x08, 0xEE);
}
