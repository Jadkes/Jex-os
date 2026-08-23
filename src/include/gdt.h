/**
 * @file gdt.h
 * @brief Global Descriptor Table (GDT) and Task State Segment (TSS) structures.
 *
 * Defines the segment descriptors for memory protection and the TSS for privilege transitions.
 */

#ifndef GDT_H
#define GDT_H

#include <stdint.h>
#include "smp.h"

/* GDT layout: index = entry number; selector = index << 3 | RPL.
 * One TSS slot per CPU follows the five base descriptors so every core
 * can LTR its own task register without colliding on a busy descriptor. */
#define GDT_BASE_ENTRIES  5
#define GDT_TSS_IDX       GDT_BASE_ENTRIES
#define GDT_DF_TSS_IDX    (GDT_BASE_ENTRIES + NR_CPUS)
#define GDT_NUM_ENTRIES   (GDT_BASE_ENTRIES + NR_CPUS + 1)

#define SEG_KERNEL_CODE   0x08
#define SEG_KERNEL_DATA   0x10
#define SEG_USER_CODE     0x0B    /* selector form: (3<<3)|RPL3 */
#define SEG_USER_DATA     0x13

/* Access bytes: present, descriptor type, ring, executable/readable */
#define SEG_ACCESS_KCODE  0x9A
#define SEG_ACCESS_KDATA  0x92
#define SEG_ACCESS_UCODE  0xFA
#define SEG_ACCESS_UDATA  0xF2
#define SEG_ACCESS_TSS    0x89    /* available 32-bit TSS, DPL=0 */

/* Granularity flags: 4 KiB granularity + 32-bit default + upper nibble */
#define SEG_FLAGS_4GB     0xCF

/**
 * @struct gdt_entry_struct
 * @brief Represents a single entry in the Global Descriptor Table.
 */
struct gdt_entry_struct
{
    uint16_t limit_low;     /**< Lower 16 bits of the limit. */
    uint16_t base_low;      /**< Lower 16 bits of the base. */
    uint8_t  base_middle;   /**< Next 8 bits of the base. */
    uint8_t  access;        /**< Access flags (ring, type, etc.). */
    uint8_t  granularity;   /**< Granularity and limit high bits. */
    uint8_t  base_high;     /**< Last 8 bits of the base. */
} __attribute__((packed));

typedef struct gdt_entry_struct gdt_entry_t;

/**
 * @struct gdt_ptr_struct
 * @brief Represents the GDTR register value passed to the LGDT instruction.
 */
struct gdt_ptr_struct
{
    uint16_t limit;         /**< The size of the GDT minus 1. */
    uint32_t base;          /**< The physical address of the GDT. */
} __attribute__((packed));

typedef struct gdt_ptr_struct gdt_ptr_t;

/* Selector of the calling CPU's TSS descriptor. */
static inline uint16_t tss_selector(int cpu) {
    return (uint16_t)((GDT_TSS_IDX + cpu) << 3);
}

/* Selector of the dedicated double-fault TSS (task gate target). */
static inline uint16_t df_tss_selector(void) {
    return (uint16_t)(GDT_DF_TSS_IDX << 3);
}

/**
 * @brief Arm the double-fault landing pad with a valid page directory.
 * Call once paging is live; a task-gate switch reloads CR3 from the
 * TSS, so it must point at the kernel directory.
 */
void df_tss_set_cr3(uint32_t cr3);

/**
 * @struct tss_entry_struct
 * @brief Task State Segment (TSS) structure.
 *
 * Used primarily for storing the kernel stack pointer (esp0) used during interrupts
 * from user mode.
 */
struct tss_entry_struct
{
    uint32_t prev_tss;      /**< Previous TSS link (unused in software task switching). */
    uint32_t esp0;          /**< Stack pointer to load when changing to kernel mode. */
    uint32_t ss0;           /**< Stack segment to load when changing to kernel mode. */
    uint32_t esp1;          /**< Unused. */
    uint32_t ss1;           /**< Unused. */
    uint32_t esp2;          /**< Unused. */
    uint32_t ss2;           /**< Unused. */
    uint32_t cr3;           /**< Unused. */
    uint32_t eip;           /**< Unused. */
    uint32_t eflags;        /**< Unused. */
    uint32_t eax;           /**< Unused. */
    uint32_t ecx;           /**< Unused. */
    uint32_t edx;           /**< Unused. */
    uint32_t ebx;           /**< Unused. */
    uint32_t esp;           /**< Unused. */
    uint32_t ebp;           /**< Unused. */
    uint32_t esi;           /**< Unused. */
    uint32_t edi;           /**< Unused. */
    uint32_t es;            /**< Unused. */
    uint32_t cs;            /**< Unused. */
    uint32_t ss;            /**< Unused. */
    uint32_t ds;            /**< Unused. */
    uint32_t fs;            /**< Unused. */
    uint32_t gs;            /**< Unused. */
    uint32_t ldt;           /**< Unused. */
    uint16_t trap;          /**< Unused. */
    uint16_t iomap_base;    /**< Offset to the I/O permission bitmask. */
} __attribute__((packed));

typedef struct tss_entry_struct tss_entry_t;

/**
 * @brief The Task State Segment instance used for ring transitions.
 */
extern tss_entry_t tss_entries[NR_CPUS];

/* Shared GDT table and IDTR-style descriptor (defined in gdt.c).
 * APs reload these in ap_startup so every CPU runs the same descriptors. */
extern gdt_entry_t gdt_entries[GDT_NUM_ENTRIES];
extern gdt_ptr_t   gdt_ptr;

/**
 * @brief Initialize the GDT and load it into the CPU.
 */
void init_gdt();

/**
 * @brief Set the kernel stack pointer in the TSS.
 * @param stack The address of the top of the kernel stack.
 */
void set_kernel_stack(uint32_t stack);

#endif // GDT_H
