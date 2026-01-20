.global jump_to_user_mode

/* jump_to_user_mode(uint32_t entry_point, uint32_t user_stack_top) */
jump_to_user_mode:
    cli
    /* Get entry_point from 4(%esp) and user_stack_top from 8(%esp) */
    mov 4(%esp), %ecx
    mov 8(%esp), %ebx

    /* Set up user mode segments */
    mov $0x23, %ax 
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %gs

    /* Push the stack frame for iret */
    push $0x23      /* User SS */
    push %ebx       /* User ESP (from parameter) */
    
    pushf           /* EFLAGS */
    pop %eax
    or $0x200, %eax /* Enable interrupts in Ring 3 */
    push %eax

    push $0x1B      /* User CS */
    push %ecx       /* User EIP (from parameter) */
    
    iret

.global default_user_start
default_user_start:
    /* This is the backup Ring 3 code if no ELF is loaded */
    mov $0, %eax
    mov $msg, %ebx
    int $0x80
    1: jmp 1b

msg:
    .asciz "\n[USER MODE] No ELF provided. Running default Ring 3 test via Syscall!\n"