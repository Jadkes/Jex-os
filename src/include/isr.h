/**
 * @file isr.h
 * @brief Interrupt Service Routine (ISR) interface.
 *
 * Defines the register state passed to C handlers after an interrupt.
 */

#ifndef ISR_H
#define ISR_H

#include <stdint.h>

/**
 * @struct registers
 * @brief State of the CPU registers during an interrupt.
 *
 * This structure matches the stack layout created by the assembly interrupt stubs.
 */
typedef struct registers
{
    uint32_t ds;                  /**< Data segment selector. */
    uint32_t edi, esi, ebp, esp, ebx, edx, ecx, eax; /**< Pushed by 'pusha'. */
    uint32_t int_no, err_code;    /**< Interrupt number and error code (if provided). */
    uint32_t eip, cs, eflags, useresp, ss; /**< Pushed by the CPU automatically. */
} registers_t;

/**
 * @brief Install the basic ISR handlers for CPU exceptions (0-31).
 */
void isr_install();

/**
 * @brief Common C handler for all interrupts.
 * @param regs The CPU state at the time of the interrupt.
 */
void isr_handler(registers_t regs);

#endif // ISR_H
