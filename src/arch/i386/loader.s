/*
 * loader.s - Stage 2 Bootloader
 *
 * Loaded at 0x7E00 by bootsect.s (stage 1).  Operates in two phases:
 *
 *   Phase 1 (real mode):
 *     - Load the kernel ELF from disk into a temporary buffer at 0x10000
 *       using BIOS INT 13h AH=42h (LBA extended reads, max 127 sec/batch).
 *     - Set up a flat GDT and switch to protected mode.
 *
 *   Phase 2 (protected mode, 32-bit):
 *     - Parse the ELF header, copy LOAD segments to their target addresses.
 *     - Zero-fill BSS segments.
 *     - Construct a minimal multiboot_info_t structure.
 *     - Jump to the kernel entry point with eax = 0x2BADB002 and ebx = &mbi.
 *     - (We never return.)
 *
 * Disk layout (after bootsect):
 *   Sectors 1-7:   loader (this file)      — stage 2
 *   Sectors 8+:    jexos.bin               — kernel ELF
 *
 * Constants marked MAY_BE_PATCHED are overwritten by the Makefile
 * with the actual kernel size / LBA at link time.
 */

.include "loader_defs.h"          /* shared defines */

/* ═══════════════════════════════════════════════════════════════════
 * Phase 1 — Real mode (16-bit)
 * ═══════════════════════════════════════════════════════════════════ */
.code16
.section .text
.globl  loader_start

loader_start:
    /* Save boot drive (passed from BIOS → bootsect → here in DL) */
    movb   %dl, drive_number

    /* Set up our own stack in conventional memory */
    xorw   %ax, %ax
    movw   %ax, %ds
    movw   %ax, %ss
    movw   $0x7E00, %sp

    /* ─── Load kernel from disk (batched LBA reads) ────────────── */
    movw   $KERNEL_LBA_LOW,  %di   /* LBA low word */
    movw   $KERNEL_LBA_HIGH, %si   /* LBA high word */
    movw   $KERNEL_SEGMENT,  %bx   /* buffer segment (0x1000) */
    movw   $KERNEL_SECTORS,  %cx   /* total sectors remaining */

.L_kload:
    /* How many sectors for this batch?  Cap at 127 for BIOS compat. */
    movw   %cx, %dx
    cmpw   $127, %dx
    jbe    .L_last_batch
    movw   $127, %dx
.L_last_batch:

    /* Fill in the Disk Address Packet for this batch */
    movw   %dx,      kernel_dap_sectors
    movw   $0x0000,  kernel_dap_offset
    movw   %bx,      kernel_dap_segment
    movw   %di,      kernel_dap_lba_0
    movw   %si,      kernel_dap_lba_2
    movl   $0,       kernel_dap_lba_4   /* upper 32 bits of LBA = 0 */

    /* Call INT 13h AH=42h */
    pushal
    movw   $kernel_dap, %si
    movb   $0x42, %ah
    int    $0x13
    jc     disk_error
    popal

    /* Advance to next batch */
    subw   %dx, %cx                  /* decrement remaining sector count */
    jz     .L_kernel_loaded

    /* LBA += batch_sectors */
    addw   %dx, %di
    adcw   $0,  %si

    /* segment += batch_sectors * 32  (512 B/sector ÷ 16 bytes per seg unit) */
    movw   %dx, %ax
    shlw   $5,  %ax
    addw   %ax, %bx

    jmp    .L_kload

.L_kernel_loaded:

    /* ─── Set up Global Descriptor Table ─────────────────────── */
    cli
    lgdt   gdt_desc

    /* Set Protection Enable bit in CR0 */
    movl   %cr0, %eax
    orl    $1,   %eax
    movl   %eax, %cr0

    /* Far jump: reload CS with 32-bit code selector 0x08.
     * The "l" suffix forces a data-size override (0x66 prefix)
     * so the offset is emitted as 32 bits even in .code16 mode. */
    ljmpl  $0x08, $pm_entry


/* ═══════════════════════════════════════════════════════════════════
 * Shared data (accessed by both real-mode and protected-mode code)
 * ═══════════════════════════════════════════════════════════════════ */
    .balign 4
kernel_dap:
    .byte  0x10           /* DAP size */
    .byte  0x00           /* reserved */
kernel_dap_sectors:
    .word  127            /* sectors per batch — MAY BE PATCHED */
kernel_dap_offset:
    .word  0x0000         /* transfer offset (within segment) */
kernel_dap_segment:
    .word  0x1000         /* transfer segment */
kernel_dap_lba_0:
    .word  0              /* LBA low  16 bits */
kernel_dap_lba_2:
    .word  0              /* LBA mid  16 bits */
kernel_dap_lba_4:
    .long  0              /* LBA high 32 bits */

drive_number:
    .byte  0

gdt:
    .quad  0x0000000000000000      /* null descriptor */
gdt_code:
    .quad  0x00CF9A000000FFFF      /* 32-bit code, base 0, limit 4 GB */
gdt_data:
    .quad  0x00CF92000000FFFF      /* 32-bit data, base 0, limit 4 GB */
gdt_end:

gdt_desc:
    .word  gdt_end - gdt - 1       /* GDT limit (bytes - 1) */
    .long  gdt                     /* GDT base (physical address) */


/* ═══════════════════════════════════════════════════════════════════
 * Phase 2 — Protected mode (32-bit)
 * ═══════════════════════════════════════════════════════════════════ */
.code32
pm_entry:

    /* Reload data segment selectors */
    movw   $0x10, %ax
    movw   %ax, %ds
    movw   %ax, %es
    movw   %ax, %fs
    movw   %ax, %gs
    movw   %ax, %ss

    /* Stack pointer (safe area, below the kernel load buffer) */
    movl   $0x7C00, %esp

    /* ─── Parse ELF at KERNEL_BUFFER (0x10000) ───────────────── */

    /* Verify ELF magic: "\x7fELF" as a little-endian u32 */
    movl   $KERNEL_BUFFER, %esi
    cmpl   $0x464C457F, (%esi)
    jne    elf_error

    /* Locate the program header table */
    movl   0x1C(%esi), %eax        /* e_phoff */
    addl   %esi, %eax              /* = KERNEL_BUFFER + e_phoff */
    movl   %eax, phdr_ptr

    movzwl 0x2C(%esi), %ecx        /* e_phnum (program header count) */

.L_segloop:
    pushl  %ecx                    /* save outer loop counter */

    movl   phdr_ptr, %esi

    /* Skip non-LOAD segments */
    movl   (%esi), %eax            /* p_type */
    cmpl   $1, %eax                /* PT_LOAD = 1 */
    jne    .L_next

    /*
     * Copy segment:  memseg(p_vaddr, KERNEL_BUFFER + p_offset, p_filesz)
     *
     * After parsing, %esi still points at the program header entry.
     */
    movl   4(%esi), %eax           /* p_offset */
    addl   $KERNEL_BUFFER, %eax    /* source = buffer + p_offset       → %esi */
    movl   8(%esi), %edi           /* dest   = p_vaddr                 → %edi */
    movl   16(%esi), %ecx          /* count  = p_filesz                → %ecx */

    /* memcpy(dest, src, filesz) */
    pushl  %esi
    movl   %eax, %esi
    rep    movsb                   /* copies %ecx bytes, advances %edi */
    popl   %esi                    /* restore phdr pointer */

    /* Zero BSS:  memset(p_vaddr + p_filesz, 0, p_memsz - p_filesz)   */
    movl   20(%esi), %ecx          /* p_memsz */
    subl   16(%esi), %ecx          /* -= p_filesz */
    jz     .L_next                 /* no BSS → skip */

    /* %edi already points at p_vaddr + p_filesz (from the rep movsb above) */
    xorl   %eax, %eax
    rep    stosb                   /* zero-fill BSS */

.L_next:
    /* Advance to next program header entry */
    movl   phdr_ptr, %eax
    addl   $32, %eax
    movl   %eax, phdr_ptr

    popl   %ecx
    loop   .L_segloop

    /* ─── Build minimal multiboot_info_t ─────────────────────── */
    /* Placed in conventional memory where the kernel won't touch it. */

    movl   $MBI_ADDR, %edi
    movl   $MBI_SIZE, %ecx
    xorl   %eax, %eax
    rep    stosb                    /* zero-fill the structure */

    movl   $0x00000001, MBI_FLAGS_OFF   /* flags: bit 0 = mem info valid */
    movl   $639,        MBI_MEM_LOW     /* mem_lower = 639 KB  */
    movl   $130048,     MBI_MEM_HIGH    /* mem_upper = 127 MB (128 MB total) */

    /* ─── Jump to kernel entry point ─────────────────────────── */

    /* Set up the calling convention expected by boot.s / _start:
     *   eax = MULTIBOOT_MAGIC_VALID (0x2BADB002)
     *   ebx = &multiboot_info
     *
     * _start then pushes both and calls kernel_main(magic, mbi).
     */
    movl   $KERNEL_BUFFER, %esi
    movl   0x18(%esi), %edi           /* e_entry from ELF header */

    movl   $0x2BADB002, %eax
    movl   $MBI_ADDR,    %ebx

    jmp    *%edi                      /* never returns */


/* ═══════════════════════════════════════════════════════════════════
 * Error stubs (32-bit text provides the actual code)
 * ═══════════════════════════════════════════════════════════════════ */
elf_error:
    /* For now: infinite loop.  The kernel panic handler would be
     * visible on serial or terminal once it initialises. */
    hlt
    jmp   elf_error

disk_error:
    /* Real-mode error; fall through to the flashing "ERR" from bootsect.
     * This stub is reached only via the int 13h carry-path in .code16,
     * so it must emit a short message via INT 10h.  But since we are
     * now in PM after a possible transition... actually this stub is
     * only reachable from the .code16 disk-read loop, which branches
     * here before switching modes.  We handle it above in .code16. */
    .code16
    /* Restore DS = 0 for BIOS calls */
    xorw   %ax, %ax
    movw   %ax, %ds
    movw   $0x0E45, %ax    /* 'E' */
    int    $0x10
    movw   $0x0E52, %ax    /* 'R' */
    int    $0x10
    movw   $0x0E52, %ax    /* 'R' */
    int    $0x10
    cli
    hlt
    jmp    disk_error

    .code32


/* ── BSS / data (must live in the same binary) ────────────────── */
    .balign 4
phdr_ptr:
    .long  0
