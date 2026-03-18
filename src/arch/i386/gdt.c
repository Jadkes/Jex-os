/**
 * @file gdt.c
 * @brief Global Descriptor Table (GDT) and TSS management.
 *
 * This file handles the setup of memory segments (Kernel/User Code/Data)
 * and the Task State Segment (TSS) used for privilege transitions.
 */

#include "gdt.h"
#include <stddef.h>

/* Assembly helper functions */
extern void gdt_flush(uint32_t);
extern void tss_flush();

/* The GDT table and pointer */
gdt_entry_t gdt_entries[6];
gdt_ptr_t   gdt_ptr;
tss_entry_t tss_entry;

/**
 * @brief Set the value of a GDT gate.
 * 
 * @param num Entry index in the GDT.
 * @param base Base address of the segment.
 * @param limit Size of the segment.
 * @param access Access flags.
 * @param gran Granularity flags.
 */
static void gdt_set_gate(int32_t num, uint32_t base, uint32_t limit, uint8_t access, uint8_t gran)
{
    gdt_entries[num].base_low    = (base & 0xFFFF);
    gdt_entries[num].base_middle = (base >> 16) & 0xFF;
    gdt_entries[num].base_high   = (base >> 24) & 0xFF;

    gdt_entries[num].limit_low   = (limit & 0xFFFF);
    gdt_entries[num].granularity = (limit >> 16) & 0x0F;

    gdt_entries[num].granularity |= (gran & 0xF0);
    gdt_entries[num].access      = access;
}

/**
 * @brief Initialize the TSS entry in the GDT.
 * 
 * @param num GDT entry index for the TSS.
 * @param ss0 Kernel stack segment selector.
 * @param esp0 Kernel stack pointer.
 */
static void write_tss(int32_t num, uint16_t ss0, uint32_t esp0)
{
    uint32_t base = (uint32_t) &tss_entry;
    uint32_t limit = base + sizeof(tss_entry);

    /* TSS descriptor: DPL=3, Type=0x09 (Available 32-bit TSS) */
    gdt_set_gate(num, base, limit, 0xE9, 0x00);

    /* Initialize TSS with zeros */
    uint8_t* ptr = (uint8_t*)&tss_entry;
    for(size_t i = 0; i < sizeof(tss_entry); i++) ptr[i] = 0;

    tss_entry.ss0  = ss0;
    tss_entry.esp0 = esp0;

    /* Set default segments for user mode transitions */
    tss_entry.cs   = 0x0b;
    tss_entry.ss = tss_entry.ds = tss_entry.es = tss_entry.fs = tss_entry.gs = 0x13;
    tss_entry.iomap_base = sizeof(tss_entry);
}

/**
 * @brief Update the kernel stack pointer in the TSS.
 * Used during task switching to ensure interrupts from user mode land on the correct stack.
 * 
 * @param stack New kernel stack top address.
 */
void set_kernel_stack(uint32_t stack)
{
    tss_entry.esp0 = stack;
}

/**
 * @brief Initialize the Global Descriptor Table.
 * Sets up Null, Kernel Code, Kernel Data, User Code, User Data segments and the TSS.
 */
void init_gdt()
{
    gdt_ptr.limit = (sizeof(gdt_entry_t) * 6) - 1;
    gdt_ptr.base  = (uint32_t)&gdt_entries;

    gdt_set_gate(0, 0, 0, 0, 0);                /* Null segment */
    gdt_set_gate(1, 0, 0xFFFFFFFF, 0x9A, 0xCF); /* Kernel Code: Base 0, Limit 4GB, Ring 0 */
    gdt_set_gate(2, 0, 0xFFFFFFFF, 0x92, 0xCF); /* Kernel Data: Base 0, Limit 4GB, Ring 0 */
    gdt_set_gate(3, 0, 0xFFFFFFFF, 0xFA, 0xCF); /* User Code: Base 0, Limit 4GB, Ring 3 */
    gdt_set_gate(4, 0, 0xFFFFFFFF, 0xF2, 0xCF); /* User Data: Base 0, Limit 4GB, Ring 3 */

    /* Initially set esp0 to 0, it will be updated in kernel_main */
    write_tss(5, 0x10, 0x0); 

    /* Load the GDT and TSS into CPU registers */
    gdt_flush((uint32_t)&gdt_ptr);
    tss_flush();
}
