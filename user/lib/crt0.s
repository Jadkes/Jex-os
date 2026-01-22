.section .text
.global _start

_start:
    /* Call main */
    call main

    /* Call exit syscall */
    mov $1, %eax
    int $0x80

    /* Safety hang */
    1: jmp 1b
