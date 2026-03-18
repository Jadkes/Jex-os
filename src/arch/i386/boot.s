/**
 * @file boot.s
 * @brief Kernel Entry Point and Multiboot Header.
 *
 * This file is the first code executed by the bootloader. It sets up the
 * initial stack, passes multiboot information to the C kernel, and
 * jumps into the kernel main function.
 */

/* Multiboot header constants */
.set ALIGN,    1<<0             /* align loaded modules on page boundaries */
.set MEMINFO,  1<<1             /* provide memory map */
.set FLAGS,    ALIGN | MEMINFO  /* this is the Multiboot 'flag' field */
.set MAGIC,    0x1BADB002       /* 'magic' number lets bootloader find the header */
.set CHECKSUM, -(MAGIC + FLAGS) /* checksum of above, to prove we are multiboot */

/* Declare a multiboot header that marks the program as a kernel */
.section .multiboot
.align 4
.long MAGIC
.long FLAGS
.long CHECKSUM

/* Allocate the initial stack for the kernel */
.section .bss
.align 16
stack_bottom:
.skip 16384 /* 16 KiB */
stack_top:

/**
 * @brief Kernel Entry Point (_start).
 */
.section .text
.global _start
.type _start, @function
_start:
	/* Set up the initial kernel stack pointer */
	mov $stack_top, %esp

    /* Push Multiboot Magic (EAX) and Info Structure (EBX) to the stack */
    /* These will be the arguments to kernel_main(uint32_t magic, multiboot_info_t* info) */
    push %ebx
    push %eax

	/* Call the C kernel entry point */
	call kernel_main

	/**
     * @brief Infinite loop.
     * If kernel_main returns, halt the CPU indefinitely.
     */
	cli
1:	hlt
	jmp 1b

/**
 * @brief Utility to read the current Instruction Pointer (EIP).
 * Note: Returns the return address of this function.
 */
.global read_eip
read_eip:
    pop %eax
    jmp *%eax

.size _start, . - _start
