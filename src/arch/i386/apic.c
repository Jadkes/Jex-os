/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * apic.c - Local APIC and I/O APIC driver
 *
 * Implements APIC initialization, interrupt routing, EOI, and IPI delivery.
 * The I/O APIC replaces the legacy 8259 PIC: after ioapic_init() + pic_disable(),
 * all hardware interrupts arrive through the APIC and must be acknowledged
 * with apic_eoi() instead of the old outb(0x20, 0x20) sequence.
 *
 * MMIO access to the LAPIC and I/O APIC registers uses the identity map
 * (paging is active by the time this code runs, and the identity map covers
 * the APIC MMIO regions at 0xFEE00000 and 0xFEC00000).
 *
 * Thread-safety: apic_init() is called once per CPU (BSP + each AP).  The
 *                LAPIC registers are per-CPU, so no locking is needed.
 *                apic_eoi() and apic_read_id() are ISR-safe.
 */

#define pr_fmt(fmt) "[APIC] " fmt
#include "apic.h"
#include "kernel/atomic.h"
#include "acpi.h"
#include "pci.h"
#include "irq.h"
#include "kernel/printk.h"
#include "ports.h"
#include "timer.h"

int using_apic = 0;

/* ── MMIO accessors ───────────────────────────────────── */

static inline void lapic_write(uint32_t reg, uint32_t val)
{
    *(volatile uint32_t*)(uintptr_t)(lapic_base + reg) = val;
    /* Read back to flush the APIC write buffer (APIC bus is slow). */
    (void)*(volatile uint32_t*)(uintptr_t)(lapic_base + reg);
}

static inline uint32_t lapic_read(uint32_t reg)
{
    return *(volatile uint32_t*)(uintptr_t)(lapic_base + reg);
}

/* I/O APIC uses indirect register access (index port + data port).
 * The I/O APIC register layout (Intel 82093AA / ICH9):
 *   IOREGSEL at offset 0x00  — write register index, then read/write at
 *   IOWIN   at offset 0x10  — data register (offset 0x04 is reserved). */
static inline void ioapic_write(uint32_t reg, uint32_t val)
{
    *(volatile uint32_t*)(uintptr_t)ioapic_base        = reg;
    *(volatile uint32_t*)(uintptr_t)(ioapic_base + IOAPIC_IOWIN) = val;
}

static inline uint32_t ioapic_read(uint32_t reg)
{
    *(volatile uint32_t*)(uintptr_t)ioapic_base = reg;
    return *(volatile uint32_t*)(uintptr_t)(ioapic_base + IOAPIC_IOWIN);
}

/* ── Local APIC init ──────────────────────────────────── */

void apic_init(void)
{
    /* Enable the local APIC via MSR and set the MMIO base from it. */
    uint64_t base_msr = rdmsr(IA32_APIC_BASE_MSR);
    base_msr |= APIC_BASE_ENABLE;
    wrmsr(IA32_APIC_BASE_MSR, base_msr);

    /* Override lapic_base from MSR — definitive source, not the MADT. */
    lapic_base = (uint32_t)(base_msr & 0xFFFFF000u);

    /*
     * Set Spurious Interrupt Vector Register.
     *   - Bit 8 = enable APIC
     *   - Vector 0xFF (spurious IRQ handler — we use a dummy vector)
     */
    lapic_write(LAPIC_SPURIOUS, 0xFF | APIC_SPURIOUS_ENABLE);

    /* Mask LINT0, LINT1, and performance counter LVT entries. */
    lapic_write(LAPIC_LVT_LINT0, (1 << 16) | (7 << 0));  /* masked, vector 7 */
    lapic_write(LAPIC_LVT_LINT1, (1 << 16) | (7 << 0));  /* masked, vector 7 */
    lapic_write(LAPIC_LVT_PCINT, (1 << 16) | (7 << 0));  /* masked, vector 7 */
    lapic_write(LAPIC_LVT_ERROR, (1 << 16) | 0xFE);      /* masked, vector 254 */

    /* Accept all interrupt priorities. */
    lapic_write(LAPIC_TPR, 0);

    /* Clear any stale error status. */
    lapic_write(LAPIC_ESR, 0);
    (void)lapic_read(LAPIC_ESR);

    using_apic = 1;
    pr_debug("LAPIC enabled (ID 0x%x)\n", apic_read_id());
}

/* ── I/O APIC init ────────────────────────────────────── */

/*
 * pirq_legacy_irq - Read the chipset's PIRQ-to-legacy-IRQ routing table.
 *
 * @pirq: PIRQ line index, 0 = A through 3 = D.
 *
 * In APIC mode the PCI INTx# lines assert GSI 16+@pirq directly, but the
 * chipset still holds the legacy routing values the firmware programmed
 * (the same table that decides which PIC IRQ a device's interrupt_line
 * field reports).  Reading it lets us route each PCI GSI to the exact
 * vector its driver already registered for, replicating PIC behaviour.
 *
 * Tries PIIX3 (i440FX pc machine) then ICH9 LPC (q35).  Returns the legacy
 * IRQ number (0-15), or -1 if unrouted / chipset not found.
 */
static int pirq_legacy_irq(int pirq)
{
    static const uint8_t bdf[2][3] = {
        { 0, 1, 0 },    /* PIIX3/PIIX4 ISA bridge on i440FX */
        { 0, 31, 0 },   /* ICH9 LPC bridge on q35 */
    };

    for (int c = 0; c < 2; c++) {
        uint32_t v = pci_config_read_dword(bdf[c][0], bdf[c][1], bdf[c][2],
                                           0x60 + (pirq & ~3u));
        uint8_t val = (v >> ((pirq & 3) * 8)) & 0xFF;
        /* Bit 7 set = routed-disabled; 0 = unset (and IRQ 0/timer is
         * never a legal PCI route); >= 16 is bogus. */
        if (!(val & 0x80) && val != 0 && val < 16)
            return val;
    }
    return -1;
}

void ioapic_init(void)
{
    uint32_t ver = ioapic_read(IOAPIC_IOAPICVER);
    int max_redir = (ver >> 16) & 0xFF;

    pr_info("I/O APIC version 0x%x, %d redirection entries\n",
            ver & 0xFF, max_redir + 1);

    /*
     * Route each IRQ to the vector it would have under the PIC.
     * IRQ 0 → vector 32 (PIT timer)
     * IRQ 1 → vector 33 (keyboard)
     * ...
     * IRQ 15 → vector 47
     *
     * Redirection entry format (low dword):
     *   bits 0-7   : vector
     *   bits 8-10  : delivery mode (000 = Fixed)
     *   bit 11     : destination mode (0 = Physical)
     *   bit 12     : delivery status (read-only)
     *   bit 13     : interrupt polarity (0 = active-high)
     *   bit 15     : trigger mode (0 = edge)
     *   bit 16     : mask (0 = unmasked)
     *
     * We route all interrupts to the BSP.  Use the LAPIC ID ACPI reported
     * for CPU 0, not a hardcoded 0 — real hardware frequently gives the
     * BSP a non-zero ID (QEMU happens to start at 0).
     */
    uint32_t bsp_dest = ((uint32_t)lapic_ids[0]) << 24;
    for (int irq = 0; irq <= max_redir && irq < 16; irq++) {
        uint32_t reg_idx = IOAPIC_REDTBL + irq * 2;

        /* Low dword: vector = 32 + IRQ number, unmasked, fixed mode. */
        uint32_t low = (uint32_t)(IRQ_VECTOR_BASE + irq);
        uint32_t high = bsp_dest;   /* bits 24-27 = destination APIC ID */

        ioapic_write(reg_idx + 1, high);   /* high dword first */
        ioapic_write(reg_idx,     low);
    }

    /*
     * PCI interrupts: in APIC mode the four PIRQ lines assert GSI 16-19
     * directly (the legacy 0x60-0x63 routing regs are ignored by hardware).
     * Without programming those entries, every PCI device — including the
     * RTL8139's RX interrupt — goes silent even though its driver happily
     * registered on the legacy vector.  Route each GSI to the vector of
     * the legacy IRQ the firmware reports for that line.
     */
    for (int pirq = 0; pirq < 4; pirq++) {
        int gsi = 16 + pirq;
        if (gsi > max_redir)
            break;

        int legacy_irq = pirq_legacy_irq(pirq);
        if (legacy_irq < 0) {
            pr_info("GSI %d (PIRQ %c): unrouted, left masked\n",
                    gsi, 'A' + pirq);
            continue;
        }

        /* PCI lines are level-triggered and active-low: bit 15 = level,
         * bit 13 = active low.  Level mode matters for shared lines — the
         * IOAPIC re-asserts after EOI while the line is still low, so a
         * second device raising the same IRQ can't be lost. */
        ioapic_write(IOAPIC_REDTBL + gsi * 2 + 1, bsp_dest);
        ioapic_write(IOAPIC_REDTBL + gsi * 2,
                     (uint32_t)(32 + legacy_irq) | (1u << 13) | (1u << 15));
        pr_info("GSI %d (PIRQ %c) -> vector %d (level/low)\n",
                gsi, 'A' + pirq, 32 + legacy_irq);
    }

    pr_info("IRQs 0-15 routed through I/O APIC\n");
}

/* ── PIC disable ──────────────────────────────────────── */

void pic_disable(void)
{
    /* Mask all IRQs on both master and slave PICs. */
    outb(0xA1, 0xFF);   /* slave  PIC data port */
    outb(0x21, 0xFF);   /* master PIC data port */
    pr_info("8259 PIC masked (all IRQs)\n");
}

/* ── EOI ──────────────────────────────────────────────── */

void apic_eoi(void)
{
    lapic_write(LAPIC_EOI, 0);
}

/* ── APIC ID ──────────────────────────────────────────── */

uint32_t apic_read_id(void)
{
    return lapic_read(LAPIC_ID) >> 24;
}

/* ── IPI delivery (for SMP) ───────────────────────────── */

/*
 * Wait for any previous IPI to finish transmission before sending
 * a new one.  Poll the ICR until the Delivery Status bit clears.
 */
static void apic_wait_icr_idle(void)
{
    while (lapic_read(LAPIC_ICR_LOW) & ICR_DELIVERY_PENDING)
        __asm__ volatile("pause");
}

/* ── Per-CPU LAPIC timer ──────────────────────────────── */

uint32_t lapic_timer_interval;  /* ticks per scheduler slice */
static volatile uint32_t lapic_timers_running;

void lapic_timer_calibrate(void)
{
    /* One-shot from 0xFFFFFFFF with divide-by-16 while system_ticks
     * advances one tenth of a second; the countdown delta is the
     * raw timer rate over that window. */
    const uint32_t WINDOW_TICKS = TICKS_PER_SEC / 10;

    lapic_write(LAPIC_TIMER_DCR, 0x3);                       /* /16 */
    lapic_write(LAPIC_LVT_TIMER, LAPIC_TIMER_VECTOR | LAPIC_LVT_MASKED);
    lapic_write(LAPIC_TIMER_ICR, 0xFFFFFFFF);

    /* system_ticks advances only from PIT interrupts, so the window
     * must be measured with IF=1 regardless of caller context.  The
     * timer LVT is masked above, so vector 48 cannot fire mid-window.
     * BSP-only by construction (APs reuse the computed interval). */
    uint32_t eflags;
    __asm__ volatile("pushfl; popl %0; sti" : "=r"(eflags));

    uint32_t start = system_ticks;
    while ((uint32_t)(system_ticks - start) < WINDOW_TICKS)
        __asm__ volatile("pause");

    __asm__ volatile("pushl %0; popfl" :: "r"(eflags));

    uint32_t elapsed = 0xFFFFFFFF - lapic_read(LAPIC_TIMER_CCR);
    if (!elapsed) {
        pr_err("LAPIC timer did not count; keeping PIT preemption\n");
        return;
    }

    /* Scale back to one PIT tick period and round up */
    lapic_timer_interval = (elapsed / WINDOW_TICKS) + 1;
    pr_info("LAPIC timer calibrated: %u ticks/slice\n",
            lapic_timer_interval);
}

void lapic_timer_arm(void)
{
    if (!lapic_timer_interval)
        return;

    lapic_write(LAPIC_TIMER_DCR, 0x3);                       /* /16 */
    lapic_write(LAPIC_LVT_TIMER, LAPIC_TIMER_VECTOR | LAPIC_LVT_PERIODIC);
    lapic_write(LAPIC_TIMER_ICR, lapic_timer_interval);

    atomic_inc(&lapic_timers_running);
}

int lapic_timer_armed(void)
{
    return lapic_timers_running > 0;
}

int lapic_timer_armed_count(void)
{
    return lapic_timers_running;
}

void apic_send_init_ipi(uint8_t lapic_id)
{
    apic_wait_icr_idle();

    /* Set destination APIC ID in ICR[63:56]. */
    lapic_write(LAPIC_ICR_HIGH, (uint32_t)lapic_id << 24);
    /* Send INIT IPI (delivery mode = 101). */
    lapic_write(LAPIC_ICR_LOW,  ICR_INIT | ICR_DEST_PHYSICAL);

    apic_wait_icr_idle();
}

void apic_send_startup_ipi(uint8_t lapic_id, uint32_t vector)
{
    apic_wait_icr_idle();

    lapic_write(LAPIC_ICR_HIGH, (uint32_t)lapic_id << 24);
    /*
     * STARTUP IPI (delivery mode = 110).  The vector is the page number
     * of the real-mode trampoline (e.g., vector 0x01 for address 0x1000).
     */
    lapic_write(LAPIC_ICR_LOW,  ICR_STARTUP | ICR_DEST_PHYSICAL |
                                (vector & 0xFF));

    apic_wait_icr_idle();
}
