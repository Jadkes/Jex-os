#include "idt.h"

/* Access to ASM function */
extern void idt_flush(uint32_t);

idt_entry_t idt_entries[256];
idt_ptr_t   idt_ptr;

void idt_set_gate(uint8_t num, uint32_t base, uint16_t sel, uint8_t flags)
{
    idt_entries[num].base_lo = base & 0xFFFF;
    idt_entries[num].base_hi = (base >> 16) & 0xFFFF;
    idt_entries[num].sel     = sel;
    idt_entries[num].always0 = 0;
    // We must uncomment the OR below when we get to using user-mode.
    // It sets the interrupt gate's privilege level to 3.
    idt_entries[num].flags   = flags /* | 0x60 */;
}

void init_idt()
{
    idt_ptr.limit = sizeof(idt_entry_t) * 256 -1;
    idt_ptr.base  = (uint32_t)&idt_entries;

    // 0x08 is the code segment offset in our GDT
    // 0x8E means: Present, Ring 0, Interrupt Gate
    // We will set the gates in isr.c because we need the ISR function addresses
    // For now, we just flush the empty table (dangerous!) or rely on isr.c to call this.
    // Actually, let's just clear the memory for now.
    // proper initialization happens in isr_install() in isr.c
    
    // We only load the pointer here. The gates are set in isr.c
    // But we need to flush it.
    idt_flush((uint32_t)&idt_ptr);
}
