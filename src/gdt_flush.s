.global gdt_flush

gdt_flush:
    /* Get the pointer to the GDT, passed as a parameter. */
    mov 4(%esp), %eax
    lgdt (%eax)        /* Load the new GDT pointer */

    /* Reload the data segment registers */
    mov $0x10, %ax     /* 0x10 is the offset in the GDT to our Data Segment */
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %fs
    mov %ax, %gs
    mov %ax, %ss

    /* Jump to the new code segment to reload CS.
       0x08 is the offset to our Code Segment. */
    jmp $0x08, $.flush
.flush:
    ret
