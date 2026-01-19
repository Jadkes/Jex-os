#include "gdt.h"
#include <stddef.h>

extern void gdt_flush(uint32_t);
extern void tss_flush();

gdt_entry_t gdt_entries[6]; // 5 existing + 1 for TSS
gdt_ptr_t   gdt_ptr;
tss_entry_t tss_entry;

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

/* Internal function to write the TSS descriptor into the GDT */
static void write_tss(int32_t num, uint16_t ss0, uint32_t esp0)
{
    uint32_t base = (uint32_t) &tss_entry;
    uint32_t limit = base + sizeof(tss_entry);

    gdt_set_gate(num, base, limit, 0xE9, 0x00);

    /* Clear the TSS */
    uint8_t* ptr = (uint8_t*)&tss_entry;
    for(size_t i = 0; i < sizeof(tss_entry); i++) ptr[i] = 0;

    tss_entry.ss0  = ss0;  /* Kernel Data Segment */
    tss_entry.esp0 = esp0; /* Kernel Stack Pointer */

    /* Set iomap_base to the size of the TSS to disable the I/O permission bitmap */
    tss_entry.iomap_base = sizeof(tss_entry);
}

void set_kernel_stack(uint32_t stack)
{
    tss_entry.esp0 = stack;
}

void init_gdt()
{
    gdt_ptr.limit = (sizeof(gdt_entry_t) * 6) - 1;
    gdt_ptr.base  = (uint32_t)&gdt_entries;

    gdt_set_gate(0, 0, 0, 0, 0);                /* Null */
    gdt_set_gate(1, 0, 0xFFFFFFFF, 0x9A, 0xCF); /* Kernel Code */
    gdt_set_gate(2, 0, 0xFFFFFFFF, 0x92, 0xCF); /* Kernel Data */
    gdt_set_gate(3, 0, 0xFFFFFFFF, 0xFA, 0xCF); /* User Code */
    gdt_set_gate(4, 0, 0xFFFFFFFF, 0xF2, 0xCF); /* User Data */

    /* We'll pass the current stack pointer to the TSS. 
       In a real OS, each process has its own kernel stack. */
    write_tss(5, 0x10, 0x0); 

    gdt_flush((uint32_t)&gdt_ptr);
    tss_flush();
}