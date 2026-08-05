/*
 * loader_defs.h - Shared constants for the stage-2 bootloader
 *
 * These are consumed by loader.s as assembly .equ directives.
 *
 * NOTE: this file is NOT a C header — it is #included by GAS.
 *       Keep it to .equ / comments only.
 */

/* ─── ELF load buffer ──────────────────────────────────────────────── */
.equ KERNEL_BUFFER,     0x10000    /* temp area for kernel ELF on disk */
.equ KERNEL_SEGMENT,    0x1000     /* segment = buffer / 16            */

/*
 * The kernel's sector count and LBA are NOT constants here: the Makefile
 * patches kernel_dap_sectors / kernel_dap_lba_{0,2} in loader.s at build
 * time, and loader.s seeds its loop registers from those patched fields.
 * Never reintroduce LBA/count placeholders here — stale values were the
 * cause of reads past the end of the boot image.
 */

/* ─── Multiboot info structure layout ──────────────────────────────── */
.equ MBI_ADDR,          0x60A      /* free conventional mem (above BDA) */
.equ MBI_SIZE,          64         /* enough for flags + mem fields     */
.equ MBI_FLAGS_OFF,     0x00       /* offset of flags within mbi        */
.equ MBI_MEM_LOW,       0x04       /* offset of mem_lower               */
.equ MBI_MEM_HIGH,      0x08       /* offset of mem_upper               */

/* ─── GDT selectors ─────────────────────────────────────────────────── */
.equ GDT_CODE_SEL,      0x08       /* 32-bit code, ring 0               */
.equ GDT_DATA_SEL,      0x10       /* 32-bit data, ring 0               */
