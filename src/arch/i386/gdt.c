/**
 * @file gdt.c
 * @brief Global Descriptor Table (GDT) and TSS management.
 */

#include "gdt.h"
#include "init.h"
#include "string.h"
#include <stddef.h>

/* Assembly helper functions */
extern void gdt_flush(uint32_t);
extern void tss_flush();

/* The GDT table and pointer */
gdt_entry_t gdt_entries[GDT_NUM_ENTRIES];
gdt_ptr_t   gdt_ptr;

/* One TSS per CPU — each core LTRs its own descriptor at startup */
tss_entry_t tss_entries[NR_CPUS];

/*
 * Double-fault landing pad.  Vector 8 gets a TASK GATE targeting this
 * TSS: if a fault fires while the current stack is unusable (kernel
 * stack overrun, bad SS), an interrupt gate could not even push its
 * frame and the CPU would triple-fault silently.  The task gate
 * switches to this known-good stack instead, so the machine reports
 * the fault and halts rather than rebooting with no logs.
 */
tss_entry_t df_tss __attribute__((aligned(16)));
static uint8_t df_stack[16 * 1024] __attribute__((aligned(16)));
extern void df_panic_stub(void);

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
static void write_tss(int32_t num, int cpu, uint16_t ss0, uint32_t esp0)
{
    tss_entry_t* tss = &tss_entries[cpu];
    uint32_t base = (uint32_t)tss;
    uint32_t limit = sizeof(*tss) - 1;

    gdt_set_gate(num, base, limit, SEG_ACCESS_TSS, 0x00);

    memset(tss, 0, sizeof(*tss));

    tss->ss0  = ss0;
    tss->esp0 = esp0;

    /* Set default segments for user mode transitions */
    tss->cs   = SEG_USER_CODE;
    tss->ss = tss->ds = tss->es = tss->fs = tss->gs = SEG_USER_DATA;
    tss->iomap_base = sizeof(*tss);
}

/**
 * @brief Update the kernel stack pointer in the TSS.
 * Used during task switching to ensure interrupts from user mode land on the correct stack.
 * 
 * @param stack New kernel stack top address.
 */
void set_kernel_stack(uint32_t stack)
{
    /* Interrupts from ring 3 on this CPU land on ITS stack */
    tss_entries[cpu_id()].esp0 = stack;
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
    for (int i = 0; i < NR_CPUS; i++)
        write_tss(GDT_TSS_IDX + i, i, SEG_KERNEL_DATA, 0x0);

    /* The #DF landing pad: kernel segments, its own stack, interrupts
     * off (eflags.IF=0), halted after reporting.  CR3 is filled in by
     * df_tss_set_cr3() once paging exists; until then a #DF would have
     * nowhere sane to run -- but paging is up within a microsecond of
     * the GDT, so the window is negligible. */
    memset(&df_tss, 0, sizeof(df_tss));
    df_tss.esp   = (uint32_t)(df_stack + sizeof(df_stack));
    df_tss.cs    = SEG_KERNEL_CODE;
    df_tss.ss = df_tss.ds = df_tss.es = df_tss.fs = df_tss.gs = SEG_KERNEL_DATA;
    df_tss.eflags = 0x00000002;
    df_tss.eip   = (uint32_t)df_panic_stub;
    df_tss.iomap_base = sizeof(df_tss);
    gdt_set_gate(GDT_DF_TSS_IDX, (uint32_t)&df_tss, sizeof(df_tss) - 1,
                 SEG_ACCESS_TSS, 0x00);

    /* Load the GDT and this CPU's (BSP) TSS into registers */
    gdt_flush((uint32_t)&gdt_ptr);
    tss_flush();
}

early_init(init_gdt);

void df_tss_set_cr3(uint32_t cr3)
{
    df_tss.cr3 = cr3;
}
