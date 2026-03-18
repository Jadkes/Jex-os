/**
 * @file irq.c
 * @brief Interrupt Request (IRQ) handling and PIC remapping.
 *
 * Manages hardware interrupts from the 8259 PIC.
 */

#include "irq.h"
#include "idt.h"
#include "ports.h"
#include <stddef.h>

/* Assembly IRQ stubs declared in interrupts.s */
extern void irq0();  extern void irq1();  extern void irq2();  extern void irq3();
extern void irq4();  extern void irq5();  extern void irq6();  extern void irq7();
extern void irq8();  extern void irq9();  extern void irq10(); extern void irq11();
extern void irq12(); extern void irq13(); extern void irq14(); extern void irq15();

/**
 * @brief Table of registered IRQ callback functions.
 */
void* irq_routines[16] =
{
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0
};

/**
 * @brief Register a custom handler for a specific hardware IRQ.
 * 
 * @param n IRQ number (0-15).
 * @param handler Callback function.
 */
void register_interrupt_handler(uint8_t n, irq_handler_t handler)
{
    irq_routines[n] = handler;
}

/**
 * @brief Remap the PIC (Programmable Interrupt Controller).
 * 
 * By default, IRQs 0-7 overlap with CPU exceptions. This remaps them
 * to the 32-47 range.
 */
void irq_remap(void)
{
    outb(0x20, 0x11); /* ICW1: Init Master */
    outb(0xA0, 0x11); /* ICW1: Init Slave */
    outb(0x21, 0x20); /* ICW2: Master offset 0x20 (32) */
    outb(0xA1, 0x28); /* ICW2: Slave offset 0x28 (40) */
    outb(0x21, 0x04); /* ICW3: Master has slave at IRQ2 */
    outb(0xA1, 0x02); /* ICW3: Slave connected to Master's IRQ2 */
    outb(0x21, 0x01); /* ICW4: 8086 mode */
    outb(0xA1, 0x01); /* ICW4: 8086 mode */
    outb(0x21, 0x0);  /* Unmask all Master IRQs */
    outb(0xA1, 0x0);  /* Unmask all Slave IRQs */
}

/**
 * @brief Initialize IRQ handling.
 * Remaps the PIC and sets up IDT gates for the 16 hardware IRQs.
 */
void init_irq()
{
    irq_remap();

    idt_set_gate(32, (uint32_t)irq0, 0x08, 0x8E);
    idt_set_gate(33, (uint32_t)irq1, 0x08, 0x8E);
    idt_set_gate(34, (uint32_t)irq2, 0x08, 0x8E);
    idt_set_gate(35, (uint32_t)irq3, 0x08, 0x8E);
    idt_set_gate(36, (uint32_t)irq4, 0x08, 0x8E);
    idt_set_gate(37, (uint32_t)irq5, 0x08, 0x8E);
    idt_set_gate(38, (uint32_t)irq6, 0x08, 0x8E);
    idt_set_gate(39, (uint32_t)irq7, 0x08, 0x8E);
    idt_set_gate(40, (uint32_t)irq8, 0x08, 0x8E);
    idt_set_gate(41, (uint32_t)irq9, 0x08, 0x8E);
    idt_set_gate(42, (uint32_t)irq10, 0x08, 0x8E);
    idt_set_gate(43, (uint32_t)irq11, 0x08, 0x8E);
    idt_set_gate(44, (uint32_t)irq12, 0x08, 0x8E);
    idt_set_gate(45, (uint32_t)irq13, 0x08, 0x8E);
    idt_set_gate(46, (uint32_t)irq14, 0x08, 0x8E);
    idt_set_gate(47, (uint32_t)irq15, 0x08, 0x8E);
}

/**
 * @brief Common C handler for IRQs.
 * Called by the assembly stubs in interrupts.s.
 * 
 * @param regs CPU state at the time of the interrupt.
 */
void irq_handler(registers_t regs)
{
    /* Execute custom handler if one is registered */
    void (*handler)(registers_t*);
    handler = irq_routines[regs.int_no - 32];
    if (handler)
    {
        handler(&regs);
    }

    /* Send End of Interrupt (EOI) signal to the PICs */
    if (regs.int_no >= 40)
    {
        outb(0xA0, 0x20); /* Slave PIC EOI */
    }
    outb(0x20, 0x20); /* Master PIC EOI */
}
