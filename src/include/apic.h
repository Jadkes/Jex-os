/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * apic.h - Local APIC and I/O APIC driver
 *
 * Purpose: Enable the local APIC on each CPU, program the I/O APIC to route
 *          hardware interrupts (replacing the legacy 8259 PIC), and provide
 *          IPI primitives for SMP boot and cross-core signalling.
 *
 * Thread-safety: apic_init() must be called once per CPU (from BSP and from
 *                each AP's startup path).  apic_eoi() is ISR-safe.
 */

#ifndef APIC_H
#define APIC_H

#include <stdint.h>

/* ── IA32_APIC_BASE MSR — enables the local APIC ──────── */
#define IA32_APIC_BASE_MSR  0x1B
#define APIC_BASE_ENABLE    (1 << 11)
#define APIC_BASE_BSP       (1 << 8)

/* ── Local APIC register offsets (from MMIO base) ─────── */
#define LAPIC_ID         0x020   /* APIC ID (bits 31-24) */
#define LAPIC_VERSION    0x030
#define LAPIC_TPR        0x080   /* Task Priority */
#define LAPIC_APR        0x090
#define LAPIC_PPR        0x0A0
#define LAPIC_EOI        0x0B0   /* End Of Interrupt */
#define LAPIC_LDR        0x0D0   /* Logical Destination */
#define LAPIC_DFR        0x0E0   /* Destination Format */
#define LAPIC_SPURIOUS   0x0F0   /* Spurious Interrupt Vector */
#define LAPIC_ESR        0x280   /* Error Status */
#define LAPIC_ICR_LOW    0x300   /* Interrupt Command (low) */
#define LAPIC_ICR_HIGH   0x310   /* Interrupt Command (high) */
#define LAPIC_LVT_TIMER  0x320
#define LAPIC_LVT_PCINT  0x340   /* Performance Counter */
#define LAPIC_LVT_LINT0  0x350
#define LAPIC_LVT_LINT1  0x360
#define LAPIC_LVT_ERROR  0x370
#define LAPIC_TIMER_ICR  0x380   /* Initial Count Register */
#define LAPIC_TIMER_CCR  0x390   /* Current Count Register */
#define LAPIC_TIMER_DCR  0x3E0   /* Divide Configuration */

/* LVT timer bits */
#define LAPIC_LVT_PERIODIC   (1 << 17)
#define LAPIC_LVT_MASKED     (1 << 16)

/* Vector for the per-CPU LAPIC timer, above the PIC IRQ range (32-47) */
#define LAPIC_TIMER_VECTOR   0x30

/* Vector for TLB shootdown IPIs (handler in mm/paging.c) */
#define TLB_IPI_VECTOR       0x31

/* Spurious Vector Register bits */
#define APIC_SPURIOUS_ENABLE  (1 << 8)

/* ICR delivery modes */
#define ICR_FIXED       0x00000000
#define ICR_LOWEST      0x00000100
#define ICR_SMI         0x00000200
#define ICR_NMI         0x00000400
/* INIT requires the Level-Assert bit (14): plain 0x500 is "INIT de-assert"
 * semantics and will not wake APs on real hardware. */
#define ICR_INIT        0x0000C500
#define ICR_STARTUP     0x00000600

/* ICR destination modes */
#define ICR_DEST_PHYSICAL   0x00000000
#define ICR_DEST_LOGICAL    0x00000800

/* ICR status */
#define ICR_DELIVERY_PENDING  (1 << 12)

/* ── I/O APIC register offsets ────────────────────────── */
/* I/O APIC uses an indirect register scheme:
 *   write reg index to IOAPIC_IOREGSEL, then read/write data at IOAPIC_IOWIN */
#define IOAPIC_IOREGSEL     0x00
#define IOAPIC_IOWIN        0x10
#define IOAPIC_IOAPICID     0x00   /* register index */
#define IOAPIC_IOAPICVER    0x01
#define IOAPIC_IOAPICARB    0x02
#define IOAPIC_REDTBL       0x10   /* first redirection entry (index) */

/* Redirection entry low-dword bits */
#define IOAPIC_RED_MASKED   (1 << 16)

/* ── Public API ───────────────────────────────────────── */

/* Set to 1 once APIC mode is active (checked by irq.c for EOI). */
extern int using_apic;

/*
 * apic_init - Enable the local APIC on the current CPU.
 *
 * Writes the IA32_APIC_BASE MSR, sets the spurious vector register
 * to enable the APIC, masks LINT0/LINT1, and clears TPR.
 */
void apic_init(void);

/*
 * ioapic_init - Program the I/O APIC to route IRQs 0-15 to vectors 32-47.
 *
 * Matches the existing IDT layout from the PIC remapping.
 */
void ioapic_init(void);

/*
 * pic_disable - Mask all IRQs on the master and slave PICs.
 *
 * After the I/O APIC is active, the legacy PICs must be disabled so
 * they don't also respond to the same IRQs.
 */
void pic_disable(void);

/*
 * apic_eoi - Write the End-Of-Interrupt register of the local APIC.
 *
 * Called from irq_handler when using_apic is set.  Replaces the
 * old PIC EOI writes to ports 0x20/0xA0.
 */
void apic_eoi(void);

/*
 * apic_read_id - Read the local APIC ID of the current CPU.
 *
 * Return: APIC ID (bits 31-24 of LAPIC ID register, shifted down).
 */
uint32_t apic_read_id(void);

/*
 * lapic_timer_calibrate - Measure the LAPIC timer against PIT ticks.
 *
 * Runs a masked one-shot from 0xFFFFFFFF across ~100ms of system_ticks,
 * then stores the periodic interval that reproduces TICKS_PER_SEC.
 * Call once on the BSP before arming any CPU.
 */
void lapic_timer_calibrate(void);

/* Ticks per PIT slice from the last calibration (0 = not calibrated).
 * Shared so APs skip straight to arming with the BSP's measurement. */
extern uint32_t lapic_timer_interval;

/*
 * lapic_timer_arm - Start this CPU's LAPIC timer in periodic mode.
 *
 * Uses the interval computed by lapic_timer_calibrate(); every core
 * calls this for itself once it may take timer interrupts.
 */
void lapic_timer_arm(void);

/*
 * lapic_timer_armed - True once at least one LAPIC timer is running.
 *
 * The PIT handler checks this to hand preemption duty over.
 */
int lapic_timer_armed(void);

/* lapic_timer_armed_count - Number of CPUs with a running LAPIC timer. */
int lapic_timer_armed_count(void);

/*
 * apic_send_init_ipi - Send an INIT IPI to a specific APIC ID.
 * @lapic_id: Destination local APIC ID.
 */
void apic_send_init_ipi(uint8_t lapic_id);

/*
 * lapic_send_fixed_ipi - Fire a fixed-delivery IPI at one LAPIC.
 * @lapic_id: Destination local APIC ID (physical).
 * @vector:   Interrupt vector the target should take.
 *
 * Returns once the message is accepted for delivery; the target's
 * handler runs asynchronously.  ISR-safe: only touches the ICR.
 */
void lapic_send_fixed_ipi(uint8_t lapic_id, uint8_t vector);

/*
 * apic_send_startup_ipi - Send a STARTUP IPI to a specific APIC ID.
 * @lapic_id: Destination local APIC ID.
 * @vector:   Startup page number (trampoline_addr / 0x1000).
 */
void apic_send_startup_ipi(uint8_t lapic_id, uint32_t vector);

/* MSR helpers */
static inline uint64_t rdmsr(uint32_t msr)
{
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

static inline void wrmsr(uint32_t msr, uint64_t val)
{
    __asm__ volatile("wrmsr" : : "a"((uint32_t)val),
                     "d"((uint32_t)(val >> 32)), "c"(msr));
}

#endif /* APIC_H */
