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

/* Allocate the initial stack */
.section .bss
.align 16
stack_bottom:
.skip 16384 /* 16 KiB */
stack_top:

/* The kernel entry point */
.section .text
.global _start
.type _start, @function
_start:
	/* Set up the stack pointer */
	mov $stack_top, %esp

    /* Push Multiboot Magic and Info Structure to stack */
    push %ebx
    push %eax

	/* Call the kernel main function */
	call kernel_main

	/* Infinite loop if the system has nothing more to do */
	cli
1:	hlt
	jmp 1b

.global read_eip
read_eip:
    pop %eax
    jmp *%eax

.size _start, . - _start
