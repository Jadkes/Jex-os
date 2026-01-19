.section .text
.extern isr_handler

/* This macro defines a dummy interrupt stub (wrapper) that pushes a
   dummy error code (0) and the interrupt number. This is used for
   interrupts that don't push an error code automatically. */
.macro ISR_NOERRCODE num
.global isr\num
isr\num:
    cli
    push $0        /* Push a dummy error code */
    push $\num     /* Push the interrupt number */
    jmp isr_common_stub
.endm

/* This macro defines an interrupt stub for interrupts that DO push
   an error code automatically. We just push the interrupt number. */
.macro ISR_ERRCODE num
.global isr\num
isr\num:
    cli
    push $\num
    jmp isr_common_stub
.endm

/* Define the first 32 ISRs */
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

.global isr128
isr128:
    cli
    push $0
    push $128
    jmp isr_common_stub

/* This acts as a common handler for all ISRs. */
isr_common_stub:
    pusha                    /* Pushes edi,esi,ebp,esp,ebx,edx,ecx,eax */

    mov %ds, %ax             /* Lower 16-bits of eax = ds. */
    push %eax                /* save the data segment descriptor */

    mov $0x10, %ax           /* load the kernel data segment descriptor */
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %fs
    mov %ax, %gs

    call isr_handler

    pop %eax                 /* reload the original data segment descriptor */
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %fs
    mov %ax, %gs

    popa                     /* Pops edi,esi,ebp... */
    add $8, %esp             /* Cleans up the pushed error code and pushed ISR number */
    sti
    iret                     /* pops 5 things at once: CS, EIP, EFLAGS, SS, and ESP */

/* IRQ handlers */
.extern irq_handler

/* This macro defines a stub for IRQs */
.macro IRQ num, idt_index
.global irq\num
irq\num:
    cli
    push $0
    push $\idt_index
    jmp irq_common_stub
.endm

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

irq_common_stub:
    pusha                    /* Pushes edi,esi,ebp,esp,ebx,edx,ecx,eax */

    mov %ds, %ax
    push %eax

    mov $0x10, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %fs
    mov %ax, %gs

    call irq_handler

    pop %ebx                 /* Different from ISR! We pop to EBX temporarily */
    mov %bx, %ds
    mov %bx, %es
    mov %bx, %fs
    mov %bx, %gs

    popa
    add $8, %esp
    sti
    iret

/* Load the IDT pointer (C-callable) */
.global idt_flush
idt_flush:
    mov 4(%esp), %eax
    lidt (%eax)
    ret
