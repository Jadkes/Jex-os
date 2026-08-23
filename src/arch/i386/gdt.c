/**
 * @file gdt.c
 * @brief Global Descriptor Table (GDT) and TSS management.
 */

#include "gdt.h"
#include "init.h"
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
    uint32_t limit = sizeof(tss_entry) - 1;

    gdt_set_gate(num, base, limit, SEG_ACCESS_TSS, 0x00);

    uint8_t* ptr = (uint8_t*)&tss_entry;
    for(size_t i = 0; i < sizeof(tss_entry); i++) ptr[i] = 0;

    tss_entry.ss0  = ss0;
    tss_entry.esp0 = esp0;

    /* Set default segments for user mode transitions */
    tss_entry.cs   = SEG_USER_CODE;
    tss_entry.ss = tss_entry.ds = tss_entry.es = tss_entry.fs = tss_entry.gs = SEG_USER_DATA;
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
/*
 * init_gdt - build the flat-model GDT: null, kernel code/data,
 * user code/data, TSS.
 */
void init_gdt()
{
    gdt_ptr.limit = (sizeof(gdt_entry_t) * GDT_NUM_ENTRIES) - 1;
    gdt_ptr.base  = (uint32_t)&gdt_entries;

    /* access byte = P|DPL|S|ExDC|RW; flags 0xCF = 4K gran, 32-bit, 4GB */
    gdt_set_gate(0, 0, 0, 0, 0);
    gdt_set_gate(1, 0, 0xFFFFFFFF, SEG_ACCESS_KCODE, SEG_FLAGS_4GB);
    gdt_set_gate(2, 0, 0xFFFFFFFF, SEG_ACCESS_KDATA, SEG_FLAGS_4GB);
    gdt_set_gate(3, 0, 0xFFFFFFFF, SEG_ACCESS_UCODE, SEG_FLAGS_4GB);
    gdt_set_gate(4, 0, 0xFFFFFFFF, SEG_ACCESS_UDATA, SEG_FLAGS_4GB);

    /* Initially set esp0 to 0, it will be updated in kernel_main */
    write_tss(GDT_TSS_IDX, SEG_KERNEL_DATA, 0x0);

    /* Load the GDT and TSS into CPU registers */
    gdt_flush((uint32_t)&gdt_ptr);
    tss_flush();
}

early_init(init_gdt);
