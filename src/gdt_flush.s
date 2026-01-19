.global gdt_flush
.global tss_flush

gdt_flush:
    mov 4(%esp), %eax
    lgdt (%eax)

    mov $0x10, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %fs
    mov %ax, %gs
    mov %ax, %ss

    jmp $0x08, $.flush
.flush:
    ret

tss_flush:
    /* Load the index of our TSS structure. 
       The index is 5, so 5 * 8 = 40 = 0x28. */
    mov $0x28, %ax
    ltr %ax
    ret