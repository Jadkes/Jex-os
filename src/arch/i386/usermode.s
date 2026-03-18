/**
 * @file usermode.s
 * @brief Privilege transition (Ring 0 to Ring 3).
 */

.global jump_to_user_mode

/**
 * @brief Switch CPU to Ring 3 and jump to a user program.
 * 
 * Uses 'iret' to simulate a return from an interrupt, which is the 
 * standard way to change privilege levels on x86.
 * 
 * @param 4(%esp) The virtual entry point of the user program.
 * @param 8(%esp) The virtual address of the user stack top.
 */
jump_to_user_mode:
    cli
    /* Get parameters from stack */
    mov 4(%esp), %ecx /* Entry Point */
    mov 8(%esp), %ebx /* User Stack */

    /* Set up User-mode Data segments (selector 0x23 = 0x20 | RPL 3) */
    mov $0x23, %ax 
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %fs
    mov %ax, %gs

    /* Construct the iret stack frame */
    push $0x23      /* User SS */
    push %ebx       /* User ESP */
    
    pushf           /* EFLAGS */
    pop %eax
    or $0x200, %eax /* Ensure interrupts are enabled in user-mode */
    push %eax

    push $0x1B      /* User CS (selector 0x18 | RPL 3) */
    push %ecx       /* User EIP */
    
    /* Execute the switch */
    iret

/**
 * @brief Default code to run in Ring 3 if no external program is provided.
 */
.global default_user_start
default_user_start:
    mov $0, %eax    /* SYS_PRINT */
    mov $msg, %ebx  /* Message address */
    int $0x80
    1: jmp 1b

msg:
    .asciz "\n[USER MODE] No ELF provided. Running default Ring 3 test via Syscall!\n"
