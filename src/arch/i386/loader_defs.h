/*
 * loader_defs.h - Shared constants for the stage-2 bootloader
 *
 * These are consumed by loader.s as assembly .equ directives.
 * The Makefile patches KERNEL_SECTORS and the LBA fields in the
 * flat binary after linking, so the values here are placeholders.
 *
 * NOTE: this file is NOT a C header — it is #included by GAS.
 *       Keep it to .equ / comments only.
 */

/* ─── ELF load buffer ──────────────────────────────────────────────── */
.equ KERNEL_BUFFER,     0x10000    /* temp area for kernel ELF on disk */
.equ KERNEL_SEGMENT,    0x1000     /* segment = buffer / 16            */

/*
 * KERNEL_SECTORS and KERNEL_LBA_{LOW,HIGH} are placeholder defaults.
 * The Makefile patches the corresponding labels in loader.s via
 * objcopy --update-section or a hex-patch step at build time.
 */
.equ KERNEL_SECTORS,    376        /* ceil(188 KB / 512)  — placeholder */
.equ KERNEL_LBA_LOW,    8          /* stage 2 fits in 8 sectors        */
.equ KERNEL_LBA_HIGH,   0

/* ─── Multiboot info structure layout ──────────────────────────────── */
.equ MBI_ADDR,          0x60A      /* free conventional mem (above BDA) */
.equ MBI_SIZE,          64         /* enough for flags + mem fields     */
.equ MBI_FLAGS_OFF,     0x00       /* offset of flags within mbi        */
.equ MBI_MEM_LOW,       0x04       /* offset of mem_lower               */
.equ MBI_MEM_HIGH,      0x08       /* offset of mem_upper               */

/* ─── GDT selectors ─────────────────────────────────────────────────── */
.equ GDT_CODE_SEL,      0x08       /* 32-bit code, ring 0               */
.equ GDT_DATA_SEL,      0x10       /* 32-bit data, ring 0               */
