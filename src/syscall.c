#include "syscall.h"
#include "idt.h"
#include "shell.h"
#include "ports.h" // Need this for outb
#include <stdint.h>

extern void terminal_writestring(const char* data);
extern void isr128();
extern void log_serial(const char* str);

void syscall_handler(registers_t *regs)
{
    if (regs->eax == 0) /* print */
    {
        terminal_writestring((const char*)regs->ebx);
    }
    else if (regs->eax == 1) /* exit */
    {
        log_serial("Process exited via syscall. Resetting stack.\n");
        terminal_writestring("\n[Program exited]\n");
        
        /* CRITICAL FIX: Send End of Interrupt (EOI) to PIC!
           Otherwise the CPU thinks we are still in an interrupt and blocks IRQ 1 (Keyboard). */
        outb(0x20, 0x20); 

        /* Reset stack to top of kernel stack, ENABLE INTERRUPTS, and jump to shell_main */
        asm volatile (
            "mov $0x10000, %%esp \n"
            "sti                 \n"
            "jmp shell_main      \n"
            : : : "memory"
        );
    }
}

void init_syscalls()
{
    idt_set_gate(0x80, (uint32_t)isr128, 0x08, 0xEE);
}