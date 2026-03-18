/**
 * @file crt0.s
 * @brief C Runtime 0 - User-space entry point.
 *
 * This small assembly stub is linked into every user application.
 * It calls the C 'main' function and ensures the process exits cleanly
 * via a system call if 'main' returns.
 */

.section .text
.global _start

_start:
    /* Call the application main function */
    /* Note: argc and argv are already on the stack if set up by the kernel */
    call main

    /**
     * @brief Exit Syscall.
     * After main returns, the exit code is usually in EAX.
     * We trigger SYS_EXIT (syscall 1).
     */
    mov $1, %eax
    int $0x80

    /* If exit fails, hang the process */
    1: jmp 1b
