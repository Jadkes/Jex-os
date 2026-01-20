#include "syscall.h"
#include "idt.h"
#include "shell.h"
#include "ports.h"
#include "fs.h"
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
        /* ... existing exit logic ... */
        log_serial("Process exited via syscall. Resetting stack.\n");
        terminal_writestring("\n[Program exited]\n");
        outb(0x20, 0x20); 
        asm volatile (
            "mov $0x10000, %%esp \n"
            "sti                 \n"
            "jmp shell_main      \n"
            : : : "memory"
        );
    }
    else if (regs->eax == 2) /* open */
    {
        regs->eax = fs_open((const char*)regs->ebx, regs->ecx);
    }
    else if (regs->eax == 3) /* read */
    {
        regs->eax = fs_read(regs->ebx, (void*)regs->ecx, regs->edx);
    }
    else if (regs->eax == 4) /* write */
    {
        regs->eax = fs_write(regs->ebx, (const void*)regs->ecx, regs->edx);
    }
    else if (regs->eax == 5) /* close */
    {
        fs_close(regs->ebx);
        regs->eax = 0;
    }
    else if (regs->eax == 6) /* seek */
    {
        regs->eax = fs_seek(regs->ebx, regs->ecx, regs->edx);
    }
}

void init_syscalls()
{
    idt_set_gate(0x80, (uint32_t)isr128, 0x08, 0xEE);
}