/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * smp.c - SMP boot and per-CPU management
 *
 * Implements the Terry A. Davis SMP design: each CPU gets its own page
 * directory (clone of the kernel's with its own recursive PDE), its own
 * kernel stack, and runs its own scheduler.  APs are woken via the
 * INIT-SIPI-SIPI sequence and start execution through a real-mode
 * trampoline copied to physical 0x1000.
 *
 * Boot flow:
 *   BSP: kernel_main() → smp_init() → for each AP:
 *     1. copy trampoline to 0x1000
 *     2. prepare per-CPU page dir and stack
 *     3. write parameters to 0x1000-0x100B
 *     4. send INIT IPI → wait → send SIPI → wait → send SIPI
 *     5. AP wakes at 0x1000 real-mode → trampoline → ap_startup()
 *
 * Thread-safety: smp_init() runs once on the BSP before interrupts are
 *                enabled.  ap_startup() runs once per AP.  cpu_id() reads
 *                the LAPIC ID (per-CPU register, safe from any context).
 */

#define pr_fmt(fmt) "[SMP] " fmt
#include "smp.h"
#include "acpi.h"
#include "apic.h"
#include "paging.h"
#include "pmm.h"
#include "gdt.h"
#include "idt.h"
#include "ports.h"
#include "kernel/printk.h"
#include <string.h>

/* ── Globals ──────────────────────────────────────────── */

cpu_t    cpus[NR_CPUS] = {0};
int      cpu_count     = 1;
uint32_t lapic_base    = 0xFEE00000u;
uint32_t ioapic_base   = 0xFEC00000u;
uint32_t lapic_ids[NR_CPUS] = {0};

/* Trampoline symbols exported from smp_trampoline.S */
extern uint8_t trampoline_start[];
extern uint8_t trampoline_end[];

/* Kernel page directory (from paging.c) */
extern page_directory_t kernel_directory;

/* Virtual address where the trampoline is copied */
#define TRAMPOLINE_ADDR  0x1000

/* SIPI vector = trampoline page number (addr >> 12) */
#define TRAMPOLINE_VECTOR        (TRAMPOLINE_ADDR >> 12)

/* INIT-SIPI timing (SDM-recommended order of magnitude):
 * INIT settle, then poll the online flag in short intervals. */
#define SIPI_INIT_DELAY_MS       10
#define SIPI_POLL_LOOPS          2000
#define SIPI_POLL_INTERVAL_US    10

/*
 * Trampoline data layout — these offsets must match smp_trampoline.s.
 */
#define TRAMP_DATA_GDT      0x10C0    /* 24 bytes: 3 GDT entries */
#define TRAMP_DATA_GDTDESC  0x10F0    /* 6 bytes: GDT descriptor */
#define TRAMP_DATA_STACK    0x10F8    /* 4 bytes: stack top (ESP) */
#define TRAMP_DATA_PAGEDIR  0x10FC    /* 4 bytes: page directory (CR3) */
#define TRAMP_DATA_ENTRY    0x1100    /* 4 bytes: ap_startup() address */

/* ── Per-CPU page directory clone ─────────────────────── */

/*
 * clone_kernel_dir - Create a per-CPU page directory from the kernel's.
 *
 * Allocates a new 4 KB page for the page directory, copies the kernel's
 * PDEs, and adjusts the recursive PDE (1022) to point to itself.  Kernel
 * page tables are shared (identity map, kernel stack area).
 *
 * Return: pointer to the new page directory (physical address).
 */
static void* clone_kernel_dir(void)
{
    void* new_dir = pmm_alloc_block();
    if (!new_dir) {
        pr_err("cannot allocate per-CPU page directory\n");
        return NULL;
    }
    memset(new_dir, 0, 4096);

    /* Copy kernel page directory entries 0-1023. */
    memcpy(new_dir, &kernel_directory, sizeof(page_directory_t));

    /* Fix the recursive PDE (1022) to point to the new directory. */
    {
        uint32_t* pde = (uint32_t*)new_dir;
        uint32_t phys = (uint32_t)(uintptr_t)new_dir;
        pde[RECURSIVE_PDE_IDX] = (phys & 0xFFFFF000) | 0x03; /* present | rw */
    }

    return new_dir;
}

/* ── CPU ID ───────────────────────────────────────────── */

int cpu_id(void)
{
    uint32_t id = apic_read_id();
    for (int i = 0; i < cpu_count; i++) {
        if (cpus[i].lapic_id == id)
            return i;
    }
    /* No match: log once so a wrong-slot lookup can't hide silently */
    static int warned = 0;
    if (!warned) {
        warned = 1;
        pr_debug("cpu_id: LAPIC id 0x%x matches no cpus[] entry!", id);
    }
    return 0;   /* fallback: assume BSP */
}

/* ── Trampoline copy ──────────────────────────────────── */

static void copy_trampoline(void)
{
    uint32_t len = (uint32_t)(uintptr_t)trampoline_end
                 - (uint32_t)(uintptr_t)trampoline_start;

    if (len > 0x1000) {
        pr_err("trampoline too big (%u bytes, max 4096)\n", len);
        return;
    }

    /* Invalidate any stale TLB entry for the trampoline page. */
    __asm__ volatile("invlpg (%0)" :: "r"(TRAMPOLINE_ADDR));

    memcpy((void*)(uintptr_t)TRAMPOLINE_ADDR, trampoline_start, len);
    pr_debug("trampoline: %u bytes copied to 0x%x\n", len, TRAMPOLINE_ADDR);

    /* Write GDT entries into the trampoline data area at 0x10C0.
     * The AP loads this GDT in real mode before entering pmode.
     * Layout: NULL (8 bytes), Code (8 bytes), Data (8 bytes). */
    uint64_t* gdt = (uint64_t*)(uintptr_t)TRAMP_DATA_GDT;
    gdt[0] = 0x0000000000000000ULL;           /* NULL descriptor */
    gdt[1] = 0x00CF9A000000FFFFULL;           /* ring-0 code, base 0, 4 GB */
    gdt[2] = 0x00CF92000000FFFFULL;           /* ring-0 data, base 0, 4 GB */

    /* Write GDT descriptor (2-byte limit + 4-byte base). */
    {
        uint16_t* limit = (uint16_t*)(uintptr_t)TRAMP_DATA_GDTDESC;
        uint32_t* base  = (uint32_t*)(uintptr_t)(TRAMP_DATA_GDTDESC + 2);
        *limit = 23;                               /* 3 entries × 8 - 1 */
        *base  = (uint32_t)TRAMP_DATA_GDT;         /* GDT physical address */
    }
}

/* ── AP boot ──────────────────────────────────────────── */

/* ── Pre-timer busy-wait delays ───────────────────────── */

/*
 * The periodic timer is initialised after SMP bring-up, so the INIT/SIPI
 * sequence cannot rely on system_ticks.  Calibrate a busy-wait against
 * the PIT instead: program channel 0 in one-shot mode and count spin
 * iterations while it drains a known interval (~1 ms at 1.193182 MHz).
 */
static uint32_t loops_per_ms;

/* Latch channel 0 and return its current countdown value. */
static uint16_t pit_read_count(void)
{
    outb(PIT_CMD, 0x00);   /* latch command, ch0 */
    return inb(PIT_CH0_DATA) | ((uint16_t)inb(PIT_CH0_DATA) << 8);
}

static void calibrate_delay(void)
{
    const uint16_t span = 1193;   /* PIT ticks ≈ 1 ms */
    uint32_t spins = 0;

    /* Ch0, lo+hi byte, mode 0 (one-shot), binary */
    outb(PIT_CMD, 0x30);
    outb(PIT_CH0_DATA, 0xFF);
    outb(PIT_CH0_DATA, 0xFF);

    uint16_t start = pit_read_count();
    while ((uint16_t)(start - pit_read_count()) < span)
        spins++;

    loops_per_ms = spins;
    pr_debug("calibrated: %u loops/ms\n", (unsigned)loops_per_ms);
}

static void ap_delay_ms(uint32_t ms)
{
    while (ms--)
        for (volatile uint32_t i = 0; i < loops_per_ms; i++)
            __asm__ volatile("pause");
}

static void ap_delay_us(uint32_t us)
{
    uint32_t loops = (loops_per_ms / 1000u) * us;
    if (loops == 0) loops = 1;
    for (volatile uint32_t i = 0; i < loops; i++)
        __asm__ volatile("pause");
}

static void wake_ap(int cpu_index)
{
    uint8_t lapic_id = (uint8_t)cpus[cpu_index].lapic_id;

    /* Write trampoline parameters (stack, page dir, entry). */
    *(volatile uint32_t*)(uintptr_t)TRAMP_DATA_STACK   = cpus[cpu_index].kstack_top;
    *(volatile uint32_t*)(uintptr_t)TRAMP_DATA_PAGEDIR =
        (uint32_t)(uintptr_t)cpus[cpu_index].page_directory;
    *(volatile uint32_t*)(uintptr_t)TRAMP_DATA_ENTRY   = (uint32_t)(uintptr_t)ap_startup;

    pr_info("waking AP %d (LAPIC ID 0x%x)\n", cpu_index, lapic_id);

    apic_send_init_ipi(lapic_id);

    /*
     * INIT-SIPI sequence per the MP/SDM specification:
     *   1. INIT IPI
     *   2. wait for the AP to process INIT
     *   3. STARTUP IPI with the trampoline page vector
     *   4. poll; send a second SIPI only if the AP has not come
     *      online — an unconditional second SIPI can reset an AP that is
     *      already executing kernel code.
     */
    ap_delay_ms(SIPI_INIT_DELAY_MS);

    apic_send_startup_ipi(lapic_id, TRAMPOLINE_VECTOR);
    for (int t = 0; t < SIPI_POLL_LOOPS && !cpus[cpu_index].online; t++)
        ap_delay_us(SIPI_POLL_INTERVAL_US);

    if (!cpus[cpu_index].online) {
        pr_debug("AP %d missed first SIPI, retrying\n", cpu_index);
        apic_send_startup_ipi(lapic_id, TRAMPOLINE_VECTOR);
        for (int t = 0; t < SIPI_POLL_LOOPS && !cpus[cpu_index].online; t++)
            ap_delay_us(SIPI_POLL_INTERVAL_US);
    }

    if (!cpus[cpu_index].online)
        pr_err("AP %d did not start\n", cpu_index);
    else
        pr_debug("AP %d: SIPI sequence complete\n", cpu_index);
}

/* ── AP startup (called by trampoline) ────────────────── */

void ap_startup(void)
{
    /* Assembly descriptor loaders from gdt_flush.s / idt_flush.s */
    extern void gdt_flush(uint32_t);
    extern void tss_flush(void);
    extern void idt_flush(uint32_t);

    /*
     * Load the kernel's GDT and IDT before anything else.
     *
     * The AP arrives here on the trampoline's minimal 3-entry GDT with its
     * IDTR still pointing at the real-mode IVT (base 0).  Any exception or
     * spurious interrupt in that state fetches a gate from garbage memory,
     * double-faults, and triple-faults — with zero diagnostics.  The kernel
     * GDT uses the same selector values as the trampoline GDT (0x08/0x10),
     * so the swap is invisible to the running code.
     *
     * NOTE: deliberately NO tss_flush()/ltr here.  There is one global TSS
     * whose GDT descriptor the BSP already marked busy via LTR — loading
     * the same selector from a second CPU raises #GP(selector).  Each CPU's
     * task register therefore stays at 0, which is fine only as long as APs
     * never take ring transitions.  Per-CPU TSS + GDT slot is required
     * before APs can schedule user tasks.
     */
    gdt_flush((uint32_t)&gdt_ptr);
    idt_flush((uint32_t)&idt_ptr);

    /* cpu_id() reads the LAPIC ID register — reachable because the
     * trampoline already installed this CPU's cloned page directory. */
    int me = cpu_id();

    /* Signal the BSP that this AP is executing kernel code.  Paired with
     * the compiler barrier so the BSP's poll loop cannot miss the store. */
    cpus[me].online = 1;
    __asm__ volatile("" ::: "memory");

    apic_init();

    pr_info("CPU %d (LAPIC ID 0x%x) alive!\n", me, cpus[me].lapic_id);

    /*
     * Enter the per-CPU idle loop.
     *
     * Interrupts stay disabled because:
     *   1. No tasks are assigned to AP queues at this point
     *   2. Device interrupts are all routed to the BSP via I/O APIC
     *   3. The spurious vector has a gate now (isr255), but there is
     *      nothing on this CPU that needs interrupts yet
     *
     * The BSP will send an IPI to the AP when it has work.
     */
    ap_scheduler_loop();
}

/* ── Per-CPU scheduler loop (simplified) ──────────────── */

void ap_scheduler_loop(void)
{
    while (1) {
        /* Halt until the BSP sends an IPI to wake us. */
        __asm__ volatile("hlt");
    }
}

/* ── Mailbox API ──────────────────────────────────────── */

int mailbox_send(int target_cpu, mailbox_slot_t* msg)
{
    if (target_cpu < 0 || target_cpu >= cpu_count)
        return -1;

    mailbox_t* mb = &cpus[target_cpu].mailbox;
    uint32_t next = (mb->head + 1) % MAILBOX_SIZE;

    if (next == mb->tail)
        return -1;   /* mailbox full */

    mb->slots[mb->head] = *msg;
    /* Compiler barrier: the slot write must be ordered before the head
     * bump, or the receiver can observe head advanced with stale data. */
    __asm__ volatile("" ::: "memory");
    mb->head = next;
    return 0;
}

int mailbox_recv(mailbox_t* mb, mailbox_slot_t* msg)
{
    if (mb->tail == mb->head)
        return 0;    /* empty */

    *msg = mb->slots[mb->tail];
    /* Compiler barrier: pair of mailbox_send's — consume before tail move. */
    __asm__ volatile("" ::: "memory");
    mb->tail = (mb->tail + 1) % MAILBOX_SIZE;
    return 1;
}

/* ── Main SMP bootstrap ───────────────────────────────── */

void smp_init(void)
{
    /*
     * Scan ACPI tables to detect the number of CPUs and their LAPIC IDs.
     * Populates cpu_count, lapic_ids[], lapic_base, ioapic_base globals.
     */
    int found = acpi_init();
    if (found < 2) {
        pr_info("ACPI: %d CPU(s) — SMP not available\n", found);
        return;
    }

    pr_info("%d CPU(s) detected, booting APs...\n", cpu_count);

    /* Calibrate busy-wait delays against the PIT before waking APs —
     * the periodic timer isn't initialised yet. */
    calibrate_delay();

    apic_init();
    ioapic_init();
    pic_disable();

    cpus[0].lapic_id      = lapic_ids[0];
    cpus[0].is_bsp        = 1;
    cpus[0].page_directory = &kernel_directory;
    cpus[0].cpu_number    = 0;

    /* Copy the trampoline to low memory before waking APs. */
    copy_trampoline();

    /* Allocate and boot each AP. */
    for (int i = 1; i < cpu_count; i++) {
        cpus[i].lapic_id   = lapic_ids[i];
        cpus[i].is_bsp     = 0;
        cpus[i].cpu_number = i;

        /* Allocate a per-CPU page directory. */
        cpus[i].page_directory = clone_kernel_dir();
        if (!cpus[i].page_directory) {
            pr_err("AP %d: cannot allocate page directory, skipping\n", i);
            continue;
        }

        /* Allocate a kernel stack (using PID-offset 10+i in PDE 1023). */
        int stack_id = 10 + i;
        uint32_t stack_base = alloc_kernel_stack(stack_id);
        if (!stack_base) {
            pr_err("AP %d: cannot allocate kernel stack, skipping\n", i);
            continue;
        }
        cpus[i].kstack_top = stack_base + KSTACK_SIZE;

        /* Wake the AP. */
        wake_ap(i);
    }

    pr_info("SMP: %d CPU(s) online\n", cpu_count);
}
