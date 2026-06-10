/**
 * @file interrupts.s
 * @brief Interrupt service routine stubs and IDT loading.
 *
 * This file contains the assembly entry points for all interrupts.
 * It ensures the CPU state is saved before calling C handlers.
 */

.section .text
.extern isr_handler

/**
 * @brief Macro for ISRs that do not provide an error code.
 * Pushes a dummy 0 and the interrupt number.
 */
.macro ISR_NOERRCODE num
.global isr\num
isr\num:
    cli
    push $0        /* Dummy error code */
    push $\num     /* Interrupt number */
    jmp isr_common_stub
.endm

/**
 * @brief Macro for ISRs that provide an error code.
 * Pushes only the interrupt number.
 */
.macro ISR_ERRCODE num
.global isr\num
isr\num:
    cli
    push $\num     /* Interrupt number */
    jmp isr_common_stub
.endm

/* Define Exception stubs (0-31) */
ISR_NOERRCODE 0
ISR_NOERRCODE 1
ISR_NOERRCODE 2
ISR_NOERRCODE 3
ISR_NOERRCODE 4
ISR_NOERRCODE 5
ISR_NOERRCODE 6
ISR_NOERRCODE 7
ISR_ERRCODE   8
ISR_NOERRCODE 9
ISR_ERRCODE   10
ISR_ERRCODE   11
ISR_ERRCODE   12
ISR_ERRCODE   13
ISR_ERRCODE   14
ISR_NOERRCODE 15
ISR_NOERRCODE 16
ISR_ERRCODE   17
ISR_NOERRCODE 18
ISR_NOERRCODE 19
ISR_NOERRCODE 20
ISR_NOERRCODE 21
ISR_NOERRCODE 22
ISR_NOERRCODE 23
ISR_NOERRCODE 24
ISR_NOERRCODE 25
ISR_NOERRCODE 26
ISR_NOERRCODE 27
ISR_NOERRCODE 28
ISR_NOERRCODE 29
ISR_ERRCODE   30
ISR_NOERRCODE 31

/**
 * @brief System Call entry point (int 0x80).
 */
.global isr128
isr128:
    cli
    push $0
    push $128
    jmp isr_common_stub

/**
 * @brief Common handler for Exceptions and Syscalls.
 */
isr_common_stub:
    pusha                    /* Save all general purpose registers */

    mov %ds, %ax             /* Save data segment */
    push %eax

    mov $0x10, %ax           /* Load Kernel Data segment descriptor */
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %fs
    mov %ax, %gs

    push %esp                /* Pass pointer to register frame */
    call isr_handler         /* Call C handler (registers_t *regs) */
    add $4, %esp             /* Pop the pointer */

    pop %eax                 /* Restore data segment */
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %fs
    mov %ax, %gs

    popa                     /* Restore registers */
    add $8, %esp             /* Clean up pushed error code and int number */
    sti
    iret                     /* Return to interrupted code */

/* IRQ handlers (Hardware Interrupts) */
.extern irq_handler

/**
 * @brief Macro for hardware IRQ stubs.
 */
.macro IRQ num, idt_index
.global irq\num
irq\num:
    cli
    push $0
    push $\idt_index
    jmp irq_common_stub
.endm

/* Define IRQ stubs (remapped to 32-47) */
IRQ   0,    32
IRQ   1,    33
IRQ   2,    34
IRQ   3,    35
IRQ   4,    36
IRQ   5,    37
IRQ   6,    38
IRQ   7,    39
IRQ   8,    40
IRQ   9,    41
IRQ   10,   42
IRQ   11,   43
IRQ   12,   44
IRQ   13,   45
IRQ   14,   46
IRQ   15,   47

/**
 * @brief Common handler for Hardware IRQs.
 */
irq_common_stub:
    pusha

    mov %ds, %ax
    push %eax

    mov $0x10, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %fs
    mov %ax, %gs

    call irq_handler

    pop %ebx
    mov %bx, %ds
    mov %bx, %es
    mov %bx, %fs
    mov %bx, %gs

    popa
    add $8, %esp
    sti
    iret

/**
 * @brief Load the IDT into the CPU.
 * @param 4(%esp) Physical address of the idt_ptr structure.
 */
.global idt_flush
idt_flush:
    mov 4(%esp), %eax
    lidt (%eax)
    ret
