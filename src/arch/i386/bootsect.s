/*
 * bootsect.s - Stage 1 MBR Bootloader
 *
 * Loaded at 0x7C00 by BIOS. Saves the boot drive, loads
 * stage 2 (loader.s) from disk via INT 13h AH=42h (LBA),
 * and jumps to it.
 *
 * Layout on disk:
 *   Sector  0:  bootsect (this file)   — 512 bytes
 *   Sectors 1-7: loader (stage 2)      — up to 3.5 KB
 *   Sector  8+: jexos.bin (kernel)     — raw ELF
 *
 * The sector counts are patched at build time by the Makefile.
 * All addresses here are as seen by real-mode code.
 */

.code16
.section .text
.globl _start

/* ──────────────────────────────────────────────────────────────
 * Entry point.  BIOS jumps here at 0x7C00 with DL = boot drive.
 * ────────────────────────────────────────────────────────────── */
_start:
    /* Save the boot drive number before we clobber anything */
    movb   %dl, drive_number

    /* Set up a minimal stack below our own load address */
    xorw   %ax, %ax
    movw   %ax, %ds
    movw   %ax, %ss
    movw   $0x7C00, %sp

    /*
     * Load stage 2 from disk via INT 13h AH=42h (extended read).
     *
     * DAP fields (set by the Makefile via objcopy --update-section):
     *   dap_sectors  = number of sectors stage 2 occupies
     *   dap_lba_low  = LBA of stage 2 (= 1, right after bootsect)
     *
     * For the first build these are compiled in as defaults;
     * the Makefile patches them if they change.
     */
    movw   $dap, %si
    movb   $0x42, %ah
    int    $0x13
    jc     load_error

    /* Jump to stage 2 (loaded at 0x7E00) */
    ljmp   $0x0000, $0x7E00


/* ──────────────────────────────────────────────────────────────
 * Error handling — flash "ERR" on screen, then halt.
 * ────────────────────────────────────────────────────────────── */
load_error:
    movw   $0x0E45, %ax      /* 'E' */
    int    $0x10
    movw   $0x0E52, %ax      /* 'R' */
    int    $0x10
    movw   $0x0E52, %ax      /* 'R' */
    int    $0x10

    cli
    hlt
    jmp    load_error


/* ──────────────────────────────────────────────────────────────
 * Disk Address Packet for INT 13h AH=42h.
 *
 * These fields can be patched at link time by the Makefile:
 *   2 bytes at dap_sectors — number of sectors for stage 2
 *   8 bytes at dap_lba    — starting LBA
 * ────────────────────────────────────────────────────────────── */
    .balign 4
dap:
    .byte  0x10            /* size of DAP (16 bytes) */
    .byte  0x00            /* reserved (zero) */
dap_sectors:
    .word  7               /* sectors to read — MAY BE PATCHED */
dap_buffer:
    .word  0x7E00          /* offset  (within segment) */
dap_segment:
    .word  0x0000          /* segment */
dap_lba:
    .quad  1               /* starting LBA — MAY BE PATCHED */

drive_number:
    .byte  0

/* ──────────────────────────────────────────────────────────────
 * MBR Partition Table (embedded, no external tool required).
 *
 * A single partition of type 0x83 (Linux) covering the whole
 * disk from sector 1 onward.  Some BIOSes require a valid
 * partition table before they will boot a USB/HDD.
 * ────────────────────────────────────────────────────────────── */
    .org   0x1B8
    .long  0x00000000       /* disk signature (optional) */
    .word  0x0000           /* reserved */

    .org   0x1BE
    .byte  0x80             /* bootable flag */
    .byte  0x00, 0x02, 0x00 /* CHS start (head 0, sector 2, cyl 0) */
    .byte  0x83             /* partition type: Linux */
    .byte  0x00, 0x20, 0x00 /* CHS end   (head 0, sector 32, cyl 0) */
    .long  0x00000001       /* LBA start (sector 1) */
    .long  0x00040000       /* LBA size  (256K sectors = 128 MB) */

    .org   0x1FE
    .word  0xAA55           /* boot signature */
