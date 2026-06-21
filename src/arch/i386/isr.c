/**
 * @file isr.c
 * @brief Interrupt Service Routines (ISR) and Exception Handling.
 *
 * Handles CPU exceptions (0-31) and system calls (0x80).
 */

#include "isr.h"
#include "idt.h"
#include "init.h"
#include "paging.h"
#include "syscall.h"
#include "terminal.h"
#include "serial.h"
#include "panic.h"
#include "debug/gdb_stub.h"
#include <stddef.h>

/* System call handler (int 0x80) */
extern void syscall_handler(registers_t *regs);

/**
 * @brief Human-readable exception messages for CPU faults.
 */
const char *exception_messages[] = {
    "Division By Zero", "Debug", "Non Maskable Interrupt", "Breakpoint",
    "Into Detected Overflow", "Out of Bounds", "Invalid Opcode", "No Coprocessor",
    "Double Fault", "Coprocessor Segment Overrun", "Bad TSS", "Segment Not Present",
    "Stack Fault", "General Protection Fault", "Page Fault", "Unknown Interrupt",
    "Coprocessor Fault", "Alignment Check", "Machine Check", "Reserved",
    "Reserved", "Reserved", "Reserved", "Reserved", "Reserved", "Reserved",
    "Reserved", "Reserved", "Reserved", "Reserved", "Reserved", "Reserved"
};

/* Assembly ISR stubs declared in interrupts.s */
extern void isr0(); extern void isr1(); extern void isr2(); extern void isr3();
extern void isr4(); extern void isr5(); extern void isr6(); extern void isr7();
extern void isr8(); extern void isr9(); extern void isr10(); extern void isr11();
extern void isr12(); extern void isr13(); extern void isr14(); extern void isr15();
extern void isr16(); extern void isr17(); extern void isr18(); extern void isr19();
extern void isr20(); extern void isr21(); extern void isr22(); extern void isr23();
extern void isr24(); extern void isr25(); extern void isr26(); extern void isr27();
extern void isr28(); extern void isr29(); extern void isr30(); extern void isr31();
extern void isr128();

/**
 * @brief Populates the IDT with ISR handler addresses.
 */
void isr_install()
{
    /* Exceptions (0-31) */
    idt_set_gate(0, (uint32_t)isr0, 0x08, 0x8E); idt_set_gate(1, (uint32_t)isr1, 0x08, 0x8E);
    idt_set_gate(2, (uint32_t)isr2, 0x08, 0x8E); idt_set_gate(3, (uint32_t)isr3, 0x08, 0x8E);
    idt_set_gate(4, (uint32_t)isr4, 0x08, 0x8E); idt_set_gate(5, (uint32_t)isr5, 0x08, 0x8E);
    idt_set_gate(6, (uint32_t)isr6, 0x08, 0x8E); idt_set_gate(7, (uint32_t)isr7, 0x08, 0x8E);
    idt_set_gate(8, (uint32_t)isr8, 0x08, 0x8E); idt_set_gate(9, (uint32_t)isr9, 0x08, 0x8E);
    idt_set_gate(10, (uint32_t)isr10, 0x08, 0x8E); idt_set_gate(11, (uint32_t)isr11, 0x08, 0x8E);
    idt_set_gate(12, (uint32_t)isr12, 0x08, 0x8E); idt_set_gate(13, (uint32_t)isr13, 0x08, 0x8E);
    idt_set_gate(14, (uint32_t)isr14, 0x08, 0x8E); idt_set_gate(15, (uint32_t)isr15, 0x08, 0x8E);
    idt_set_gate(16, (uint32_t)isr16, 0x08, 0x8E); idt_set_gate(17, (uint32_t)isr17, 0x08, 0x8E);
    idt_set_gate(18, (uint32_t)isr18, 0x08, 0x8E); idt_set_gate(19, (uint32_t)isr19, 0x08, 0x8E);
    idt_set_gate(20, (uint32_t)isr20, 0x08, 0x8E); idt_set_gate(21, (uint32_t)isr21, 0x08, 0x8E);
    idt_set_gate(22, (uint32_t)isr22, 0x08, 0x8E); idt_set_gate(23, (uint32_t)isr23, 0x08, 0x8E);
    idt_set_gate(24, (uint32_t)isr24, 0x08, 0x8E); idt_set_gate(25, (uint32_t)isr25, 0x08, 0x8E);
    idt_set_gate(26, (uint32_t)isr26, 0x08, 0x8E); idt_set_gate(27, (uint32_t)isr27, 0x08, 0x8E);
    idt_set_gate(28, (uint32_t)isr28, 0x08, 0x8E); idt_set_gate(29, (uint32_t)isr29, 0x08, 0x8E);
    idt_set_gate(30, (uint32_t)isr30, 0x08, 0x8E); idt_set_gate(31, (uint32_t)isr31, 0x08, 0x8E);
    
    /* System Call (int 0x80) - DPL set to 3 to allow user mode access */
    idt_set_gate(128, (uint32_t)isr128, 0x08, 0xEE);
}

early_init(isr_install);

/**
 * @brief Main C handler for all ISRs.
 *
 * Now accepts a pointer so that handlers (e.g. GDB stub) can modify
 * the register save area in the interrupt stack frame, and those
 * changes are visible after iret.
 *
 * Routes system calls to the syscall handler, page faults to the paging manager,
 * debug/breakpoint events to the GDB stub, and halts on unhandled exceptions.
 */
void isr_handler(registers_t *regs)
{
    /* Handle System Call */
    if (regs->int_no == 128) {
        syscall_handler(regs);
        return;
    }

    /* Handle Page Fault */
    if (regs->int_no == 14) {
        page_fault_handler(regs);
        return;
    }

    /* Debug exception (int 1) — currently only handles single-step (TF flag) */
    if (regs->int_no == 1) {
        if (regs->eflags & 0x100) {
            gdb_stub_handle_trace(regs);
        }
        /* Other debug events: ignore and return */
        return;
    }

    /* Breakpoint (int 3) — route to GDB stub */
    if (regs->int_no == 3) {
        gdb_stub_handler(regs);
        return;
    }

    /* Delegate to the comprehensive panic handler (register dump, stack trace, recovery) */
    panic_handler(regs);
    for(;;) asm volatile ("hlt");   /* Safety net — panic_handler shouldn't return */
}
