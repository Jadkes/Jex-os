/**
 * @file irq.h
 * @brief Interrupt Request (IRQ) management.
 *
 * Handles external hardware interrupts (via PIC) and custom registration.
 */

#ifndef IRQ_H
#define IRQ_H

#include "isr.h"

/**
 * @brief Callback type for IRQ handlers.
 */
typedef void (*irq_handler_t)(registers_t*);

/**
 * @brief Initialize the Programmable Interrupt Controller (PIC) and remap IRQs.
 */
void init_irq();

/**
 * @brief Register a specific handler for an interrupt.
 * 
 * @param n Interrupt number (0-255).
 * @param handler Function to call when the interrupt occurs.
 */
void register_interrupt_handler(uint8_t n, irq_handler_t handler);

#endif // IRQ_H
