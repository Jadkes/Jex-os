/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * smp_trampoline.s - AP real-mode trampoline
 *
 * Copied to physical 0x1000 by smp_init().  Each AP starts here in
 * real mode after receiving a STARTUP IPI (vector 0x01).
 *
 * The BSP pre-populates the data area in the trampoline page:
 *   0x10C0: GDT entries          (24 bytes, 3 × 8-byte descriptors)
 *   0x10F0: GDT descriptor       (6 bytes: 2 limit + 4 base)
 *   0x10F8: stack top            (4 bytes: ESP value)
 *   0x10FC: page directory       (4 bytes: CR3 value)
 *   0x1100: entry point          (4 bytes: ap_startup address)
 *
 * After the trampoline enters protected mode + paging, it calls
 * ap_startup() and never returns.
 */

.equ DATA_GDT,     0x10C0    /* GDT entries */
.equ DATA_GDTDESC, 0x10F0    /* GDT descriptor (limit + base) */
.equ DATA_STACK,   0x10F8    /* stack pointer */
.equ DATA_PAGEDIR, 0x10FC    /* CR3 value */
.equ DATA_ENTRY,   0x1100    /* entry point address */

.code16

.global trampoline_start
trampoline_start:
    cli
    cld

    /* Enable A20 gate */
    inb    $0x92, %al
    orb    $0x02, %al
    outb   %al, $0x92

    /*
     * Load GDT descriptor from the data area.
     * In real mode with DS=0, address 0x10F0 = physical 0x10F0,
     * where the BSP wrote the descriptor before sending SIPI.
     * The `l` suffix adds the 0x66 prefix for 32-bit operand size
     * (needed for 32-bit GDT base address).
     */
    lgdtl  (DATA_GDTDESC)

    /* Enter protected mode */
    movl   %cr0, %eax
    orl    $1, %eax
    movl   %eax, %cr0

    /*
     * Far jump to 32-bit protected mode.
     * We need a 32-bit offset so the target (trampoline_pmode at
     * kernel-space address ~0x00100xxx) fits.  In .code16, a plain
     * ljmp would use a 16-bit offset.  Encode manually:
     *   0x66 = operand-size prefix (promotes offset to 32 bits)
     *   0xEA = far jump opcode
     */
    .byte  0x66, 0xEA
    .long  trampoline_pmode
    .word  0x08              /* ring-0 code segment selector */

.code32
trampoline_pmode:
    /* Set up data segments */
    mov    $0x10, %ax
    mov    %ax, %ds
    mov    %ax, %es
    mov    %ax, %ss

    /* Load stack pointer from data page */
    mov    (DATA_STACK), %esp

    /* Load page directory and enable paging.
     * CR4.PSE must be set BEFORE CR0.PG: with PSE off the CPU decodes
     * any PS=1 PDE as a page table instead of a 4MB page — instant
     * page fault while no IDT is loaded yet (triple fault). */
    mov    %cr4, %eax
    or     $0x00000010, %eax
    mov    %eax, %cr4

    mov    (DATA_PAGEDIR), %eax
    mov    %eax, %cr3

    mov    %cr0, %eax
    or     $0x80000000, %eax
    mov    %eax, %cr0

    /* Call AP startup function */
    mov    (DATA_ENTRY), %eax
    call   *%eax

    /* Halt if ap_startup ever returns */
1:  cli
    hlt
    jmp    1b

.global trampoline_end
trampoline_end:
