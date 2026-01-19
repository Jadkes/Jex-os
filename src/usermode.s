.global jump_to_user_mode

jump_to_user_mode:
    cli
    mov $0x23, %ax 
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %fs
    mov %ax, %gs

    mov %esp, %eax
    push $0x23
    push %eax
    pushf
    
    pop %eax
    or $0x200, %eax
    push %eax

    push $0x1B
    lea user_start, %eax
    push %eax
    iret

user_start:
    /* TEST SYSCALL: 
       EAX = 0 (print)
       EBX = pointer to string */
    mov $0, %eax
    mov $msg, %ebx
    int $0x80

    /* Loop forever in user mode */
    1: jmp 1b

msg:
    .asciz "\n[USER MODE] Hello from Ring 3 via Syscall!\n"