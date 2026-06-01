/**
 * @file syscall.c
 * @brief System Call Dispatcher.
 *
 * Implements the handler for 'int 0x80' which allows user-space programs
 * to request services from the kernel.
 */

#include "syscall.h"
#include "idt.h"
#include "shell.h"
#include "ports.h"
#include "fs.h"
#include <stdint.h>

extern void terminal_writestring(const char* data);
extern void isr128();

/**
 * @brief Main System Call Handler.
 * Dispatched by isr_handler in isr.c when int_no is 128 (0x80).
 * 
 * @param regs State of CPU registers containing syscall number and arguments.
 */
void syscall_handler(registers_t *regs)
{
    /* The syscall number is passed in EAX */
    if (regs->eax == SYS_PRINT) 
    {
        /* EBX = Pointer to string */
        terminal_writestring((const char*)regs->ebx);
    }
    else if (regs->eax == SYS_EXIT) 
    {
        /* Simple exit: jump back to the shell loop */
        /* In a full OS, this would destroy the process and switch tasks */
        outb(0x20, 0x20); /* Send EOI to PIC */
        asm volatile (
            "mov $0x10000, %%esp \n"
            "sti                 \n"
            "jmp shell_loop      \n"
            : : : "memory"
        );
    }
    else if (regs->eax == SYS_OPEN) 
    {
        /* EBX = filename, ECX = flags */
        regs->eax = fs_open((const char*)regs->ebx, regs->ecx);
    }
    else if (regs->eax == SYS_READ) 
    {
        /* EBX = fd, ECX = buffer, EDX = size */
        regs->eax = fs_read(regs->ebx, (void*)regs->ecx, regs->edx);
    }
    else if (regs->eax == SYS_WRITE) 
    {
        /* EBX = fd, ECX = buffer, EDX = size */
        regs->eax = fs_write(regs->ebx, (const void*)regs->ecx, regs->edx);
    }
    else if (regs->eax == SYS_CLOSE) 
    {
        /* EBX = fd */
        fs_close(regs->ebx);
        regs->eax = 0;
    }
    else if (regs->eax == SYS_SEEK) 
    {
        /* EBX = fd, ECX = offset, EDX = whence */
        regs->eax = fs_seek(regs->ebx, regs->ecx, regs->edx);
    }
    else if (regs->eax == SYS_SBRK) 
    {
        /* EBX = increment */
        extern void* sbrk(intptr_t increment);
        regs->eax = (uint32_t)sbrk(regs->ebx);
    }
    else if (regs->eax == SYS_EXECVE) 
    {
        /* EBX = filename, ECX = argv, EDX = envp */
        const char* filename = (const char*)regs->ebx;
        char** argv = (char**)regs->ecx;
        char** envp = (char**)regs->edx;
        
        extern int execve_file(const char* filename, char** argv, char** envp);
        regs->eax = execve_file(filename, argv, envp);
    }
    else if (regs->eax == SYS_FORK) 
    {
        regs->eax = -1; /* Not fully implemented */
    }
    else if (regs->eax == SYS_WAITPID) 
    {
        regs->eax = -1; /* Not fully implemented */
    }
    else if (regs->eax == SYS_PRINT_INT)
    {
        int val = (int)regs->ebx;
        char buf[32];
        int i = 0;
        int is_neg = 0;
        if (val == 0) {
            terminal_writestring("0");
        } else {
            if (val < 0) {
                is_neg = 1;
                val = -val;
            }
            while (val > 0) {
                buf[i++] = (val % 10) + '0';
                val /= 10;
            }
            if (is_neg) buf[i++] = '-';
            char rev[32];
            for (int j = 0; j < i; j++) {
                rev[j] = buf[i - 1 - j];
            }
            rev[i] = '\0';
            terminal_writestring(rev);
        }
    }
    else if (regs->eax == SYS_PRINT_CHAR)
    {
        char c = (char)regs->ebx;
        char str[2] = {c, '\0'};
        terminal_writestring(str);
    }
}

/**
 * @brief Register the syscall handler in the IDT.
 */
void init_syscalls()
{
    /* Use gate 0x80, selector 0x08, flags 0xEE (User mode access) */
    idt_set_gate(0x80, (uint32_t)isr128, 0x08, 0xEE);
}
