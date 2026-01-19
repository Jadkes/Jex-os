.global jump_to_user_mode

/* jump_to_user_mode(uint32_t user_stack_top) */
jump_to_user_mode:
    cli
    /* Get the user stack from the parameter (at 4(%esp)) */
    mov 4(%esp), %ebx

    /* Set up user mode segments */
    mov $0x23, %ax 
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %fs
    mov %ax, %gs

    /* Push the stack frame for iret */
    push $0x23      /* User SS */
    push %ebx       /* User ESP (from parameter) */
    
    pushf           /* EFLAGS */
    pop %eax
    or $0x200, %eax /* Enable interrupts in Ring 3 */
    push %eax

    push $0x1B      /* User CS */
    lea user_start, %eax
    push %eax       /* User EIP */
    
    iret

user_start:
    /* We are now in RING 3! */
    mov $0, %eax
    mov $msg, %ebx
    int $0x80

    /* Loop forever in user mode */
    1: jmp 1b

msg:
    .asciz "\n[USER MODE] Hello from Ring 3 via Syscall!\n"
