/**
 * @file idt.h
 * @brief Interrupt Descriptor Table (IDT) structures and functions.
 *
 * Configures how the CPU handles interrupts and exceptions.
 */

#ifndef IDT_H
#define IDT_H

#include <stdint.h>

/**
 * @struct idt_entry_struct
 * @brief Represents a single entry in the Interrupt Descriptor Table.
 */
struct idt_entry_struct
{
    uint16_t base_lo;       /**< Lower 16 bits of the handler address. */
    uint16_t sel;           /**< Kernel segment selector (usually 0x08). */
    uint8_t  always0;       /**< Reserved, must be 0. */
    uint8_t  flags;         /**< Attributes (Gate type, DPL, Present bit). */
    uint16_t base_hi;       /**< Upper 16 bits of the handler address. */
} __attribute__((packed));

typedef struct idt_entry_struct idt_entry_t;

/**
 * @struct idt_ptr_struct
 * @brief Represents the IDTR register value passed to the LIDT instruction.
 */
struct idt_ptr_struct
{
    uint16_t limit;         /**< Size of the IDT minus 1. */
    uint32_t base;          /**< The physical address of the IDT array. */
} __attribute__((packed));

typedef struct idt_ptr_struct idt_ptr_t;

/**
 * @brief Initialize the IDT and load it into the CPU.
 */
void init_idt();

/**
 * @brief Set an entry (gate) in the IDT.
 * 
 * @param num The interrupt number (0-255).
 * @param base The address of the interrupt handler function.
 * @param sel The kernel segment selector.
 * @param flags The gate attributes.
 */
void idt_set_gate(uint8_t num, uint32_t base, uint16_t sel, uint8_t flags);

#endif // IDT_H
