#ifndef IDT_H
#define IDT_H

#include <stdint.h>

/* Defines an IDT entry */
struct idt_entry_struct
{
    uint16_t base_lo;             // The lower 16 bits of the address to jump to
    uint16_t sel;                 // Kernel segment selector
    uint8_t  always0;             // This must always be 0
    uint8_t  flags;               // Flags (Present, Ring 0, etc)
    uint16_t base_hi;             // The upper 16 bits of the address to jump to
} __attribute__((packed));

typedef struct idt_entry_struct idt_entry_t;

/* A pointer to the array of interrupt handlers.
   Assembly instruction 'lidt' will read it */
struct idt_ptr_struct
{
    uint16_t limit;
    uint32_t base;                // The address of the first element in our idt_entry_t array.
} __attribute__((packed));

typedef struct idt_ptr_struct idt_ptr_t;

void init_idt();
void idt_set_gate(uint8_t num, uint32_t base, uint16_t sel, uint8_t flags);

#endif
