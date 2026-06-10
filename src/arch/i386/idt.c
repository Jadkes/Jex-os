/**
 * @file idt.c
 * @brief Interrupt Descriptor Table (IDT) implementation.
 *
 * Configures the IDT entries and provides the idt_set_gate helper.
 */

#include "idt.h"
#include "init.h"

/* Assembly helper to load the IDTR */
extern void idt_flush(uint32_t);

/* The IDT table (256 gates) */
idt_entry_t idt_entries[256];
idt_ptr_t   idt_ptr;

/**
 * @brief Set a gate in the IDT.
 * 
 * @param num Interrupt number (0-255).
 * @param base Handler function address.
 * @param sel Segment selector (usually 0x08).
 * @param flags Gate attributes (Type, DPL, Present).
 */
void idt_set_gate(uint8_t num, uint32_t base, uint16_t sel, uint8_t flags)
{
    idt_entries[num].base_lo = base & 0xFFFF;
    idt_entries[num].base_hi = (base >> 16) & 0xFFFF;
    idt_entries[num].sel     = sel;
    idt_entries[num].always0 = 0;
    
    /* Flags: [P] [DPL (2 bits)] [0] [Type (4 bits)] */
    idt_entries[num].flags   = flags;
}

/**
 * @brief Load the IDT into the CPU.
 * Note: Gates are actually populated in isr_install() in isr.c.
 */
void init_idt()
{
    idt_ptr.limit = sizeof(idt_entry_t) * 256 - 1;
    idt_ptr.base  = (uint32_t)&idt_entries;

    /* Load the IDTR */
    idt_flush((uint32_t)&idt_ptr);
}

early_init(init_idt);
