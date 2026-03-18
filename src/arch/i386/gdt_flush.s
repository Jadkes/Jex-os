/**
 * @file gdt_flush.s
 * @brief Low-level GDT and TSS loading.
 */

.global gdt_flush
.global tss_flush

/**
 * @brief Load the GDT into the CPU.
 * @param 4(%esp) The physical address of the gdt_ptr structure.
 */
gdt_flush:
    mov 4(%esp), %eax
    lgdt (%eax)

    /* Reload segment registers with the new Kernel Data selector (0x10) */
    mov $0x10, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %fs
    mov %ax, %gs
    mov %ax, %ss

    /* Far jump to reload CS with the new Kernel Code selector (0x08) */
    jmp $0x08, $.flush
.flush:
    ret

/**
 * @brief Load the Task Register (TR) with the TSS selector.
 */
tss_flush:
    /* The TSS is the 6th entry in our GDT, so index 5. */
    /* 5 * 8 = 40 = 0x28. Bits 0-1 are RPL=0, bit 2 is TI=0. */
    mov $0x28, %ax
    ltr %ax
    ret
