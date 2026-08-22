/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * acpi.c - ACPI table parser
 *
 * Scans the BIOS read-only memory area (0xE0000-0xFFFFF) for the RSDP
 * ("RSD PTR "), walks the RSDT to find the MADT (Multiple APIC Description
 * Table), and extracts the number of CPUs, their LAPIC IDs, and the
 * I/O APIC MMIO base address.
 *
 * All memory access is through the identity map, which is active by the
 * time this runs (paging is initialized during initcalls_run() and the
 * identity map covers 0-512 MB).
 *
 * Thread-safety: Called once during boot; not re-entrant.
 */

#define pr_fmt(fmt) "[ACPI] " fmt
#include "acpi.h"
#include "apic.h"
#include "kernel/printk.h"

/* ── Globals defined in smp.c, shared via acpi.h ─────── */
extern int     cpu_count;
extern uint32_t lapic_base;
extern uint32_t ioapic_base;
extern uint32_t lapic_ids[];

/* ── Helpers for reading physical memory ──────────────── */

static inline uint8_t  rd8(uint32_t phys) {
    return *(volatile uint8_t*)(uintptr_t)phys;
}
static inline uint16_t rd16(uint32_t phys) {
    return *(volatile uint16_t*)(uintptr_t)phys;
}
static inline uint32_t rd32(uint32_t phys) {
    return *(volatile uint32_t*)(uintptr_t)phys;
}

/* ── RSDP scan ────────────────────────────────────────── */

/*
 * rsdp_valid - Verify an RSDP candidate via its ACPI checksums.
 *
 * ACPI v1: the sum of the first 20 bytes must be 0 modulo 256.
 * ACPI v2+: the whole table (RSDP.length bytes) must also checksum to
 * zero, so validate that too when revision >= 2.
 *
 * Without this, any "RSD PTR " byte pattern in RAM (e.g. a buffer that
 * happens to contain the signature) would be accepted as the RSDP and
 * yield garbage table pointers.
 */
static int rsdp_valid(uint32_t addr)
{
    uint8_t sum = 0;
    for (int i = 0; i < 20; i++)
        sum += rd8(addr + i);
    if (sum != 0)
        return 0;

    uint8_t revision = rd8(addr + 15);
    if (revision >= 2) {
        uint32_t len = rd32(addr + 20);
        if (len < 36 || len > 0x1000)
            return 0;
        sum = 0;
        for (uint32_t i = 0; i < len; i++)
            sum += rd8(addr + i);
        if (sum != 0)
            return 0;
    }
    return 1;
}

/*
 * scan_rsdp_range - Look for a checksum-valid RSDP in [start, end).
 *
 * Candidates are 16-byte aligned per the ACPI spec.  Returns the physical
 * address, or 0 if none validates.
 */
static uint32_t scan_rsdp_range(uint32_t start, uint32_t end)
{
    for (uint32_t addr = start; addr < end; addr += 16) {
        if (rd8(addr + 0) == 'R' && rd8(addr + 1) == 'S' &&
            rd8(addr + 2) == 'D' && rd8(addr + 3) == ' ' &&
            rd8(addr + 4) == 'P' && rd8(addr + 5) == 'T' &&
            rd8(addr + 6) == 'R' && rd8(addr + 7) == ' ' &&
            rsdp_valid(addr))
            return addr;
    }
    return 0;
}

/*
 * acpi_find_rsdp - Locate the RSDP following the ACPI spec search order:
 * top 1 KB of the EBDA first, then the BIOS FSEG (0xE0000-0xFFFFF).
 *
 * The old full 0-1 MB sweep also covered the IVT/BDA, where a stray
 * signature would have been accepted without validation.
 */
static uint32_t acpi_find_rsdp(void)
{
    uint16_t ebda_seg = rd16(0x40E);   /* BDA: EBDA base segment */
    if (ebda_seg != 0) {
        uint32_t ebda = ((uint32_t)ebda_seg) << 4;
        uint32_t found = scan_rsdp_range(ebda, ebda + 0x400);
        if (found) return found;
    }

    return scan_rsdp_range(0xE0000u, 0x100000u);
}

/* ── RSDT walk ─────────────────────────────────────────── */

/*
 * acpi_find_madt - Walk the RSDT looking for a table with "APIC" signature.
 *
 * @rsdt_addr: physical address of the RSDT.
 * Return: physical address of the MADT, or 0.
 */
static uint32_t acpi_find_madt(uint32_t rsdt_addr)
{
    uint32_t rsdt_len = rd32(rsdt_addr + 4);   /* SDT.length field */
    uint32_t num_entries = (rsdt_len - 36) / 4; /* 36 = sizeof(acpi_sdt_t) */

    for (uint32_t i = 0; i < num_entries; i++) {
        uint32_t sdt_addr = rd32(rsdt_addr + 36 + i * 4);
        if (rd8(sdt_addr + 0) == 'A' && rd8(sdt_addr + 1) == 'P' &&
            rd8(sdt_addr + 2) == 'I' && rd8(sdt_addr + 3) == 'C') {
            pr_info("MADT at 0x%x\n", sdt_addr);
            return sdt_addr;
        }
    }
    return 0;
}

/* ── MADT parse ────────────────────────────────────────── */

/*
 * acpi_parse_madt - Parse the MADT for CPU entries and I/O APIC info.
 *
 * @madt_addr: physical address of the MADT.
 *
 * Iterates through the APIC entry list following the MADT header.
 * For each LAPIC entry (type 0) that is enabled (flags & 1), adds
 * the LAPIC ID to lapic_ids[].  For each I/O APIC entry (type 1),
 * records the MMIO base address.
 */
static void acpi_parse_madt(uint32_t madt_addr)
{
    /* Read the MADT header fields individually using raw byte access.
     * We skip the full struct copy to avoid any struct-layout issues. */
    uint32_t madt_len       = rd32(madt_addr + 4);   /* SDT.length */
    uint32_t madt_lapic_addr = rd32(madt_addr + 36); /* offset 36 in MADT */

    /*
     * Only trust the MADT's local APIC address if it looks reasonable.
     * The definitive value comes from MSR 0x1B (IA32_APIC_BASE), but
     * the MADT value is a useful cross-check.  The standard LAPIC base
     * is 0xFEE00000.
     */
    if (madt_lapic_addr == 0xFEE00000u || madt_lapic_addr == 0) {
        lapic_base = 0xFEE00000u;
    } else {
        /* MADT gives a non-standard value — trust it if it's above 1 MB. */
        if (madt_lapic_addr > 0x100000u)
            lapic_base = madt_lapic_addr;
        else
            lapic_base = 0xFEE00000u;
    }
    pr_info("Local APIC base = 0x%x\n", lapic_base);

    uint32_t offset   = 44;  /* MADT header = 36 (SDT) + 4 (local_apic_addr) + 4 (flags) */
    int cpus_found = 0;

    while (cpus_found < MAX_CPUS && offset + 1 < madt_len) {
        uint8_t type   = rd8(madt_addr + offset);
        uint8_t length = rd8(madt_addr + offset + 1);

        if (length < 2 || offset + length > madt_len)
            break;

        if (type == 0 && length >= 8) {
            /* Local APIC entry: type(1) + length(1) + acpi_id(1) + apic_id(1) + flags(4) */
            uint8_t acpi_id = rd8(madt_addr + offset + 2);
            uint8_t apic_id = rd8(madt_addr + offset + 3);
            uint32_t flags  = rd32(madt_addr + offset + 4);

            if (flags & 1) {   /* processor enabled */
                lapic_ids[cpus_found] = apic_id;
                pr_info("CPU %d: LAPIC ID 0x%x (ACPI ID %u)\n",
                        cpus_found, (unsigned)apic_id, (unsigned)acpi_id);
                cpus_found++;
            }
        } else if (type == 1 && length >= 12) {
            /* I/O APIC entry: type(1) + length(1) + ioapic_id(1) + reserved(1) + addr(4) + gsi_base(4) */
            uint32_t ioapic_addr = rd32(madt_addr + offset + 4);
            uint32_t gsi_base    = rd32(madt_addr + offset + 8);

            if (ioapic_addr > 0x100000u)
                ioapic_base = ioapic_addr;
            else
                ioapic_base = 0xFEC00000u;

            pr_info("I/O APIC at 0x%x (GSI base %u)\n",
                    ioapic_base, gsi_base);
        }
        /* Silently skip other types (ISO, NMI, etc.) */
        offset += length;
    }

    cpu_count = cpus_found < 1 ? 1 : cpus_found;
}

/* ── Public API ────────────────────────────────────────── */

int acpi_init(void)
{
    uint32_t rsdp_addr = acpi_find_rsdp();
    if (!rsdp_addr) {
        pr_info("RSDP not found — assuming 1 CPU\n");
        lapic_ids[0] = 0;
        lapic_base   = 0xFEE00000u;
        ioapic_base  = 0xFEC00000u;
        return 1;
    }

    /* Read RSDP fields byte-by-byte: signature(8) + checksum(1) + oem_id(6) + revision(1) */
    uint8_t revision = rd8(rsdp_addr + 15);
    uint32_t rsdt_addr = rd32(rsdp_addr + 16);

    char oem[7];
    oem[0] = rd8(rsdp_addr + 9);
    oem[1] = rd8(rsdp_addr + 10);
    oem[2] = rd8(rsdp_addr + 11);
    oem[3] = rd8(rsdp_addr + 12);
    oem[4] = rd8(rsdp_addr + 13);
    oem[5] = rd8(rsdp_addr + 14);
    oem[6] = '\0';

    pr_info("RSDP v%u at 0x%x (%s) rsdt=0x%x\n",
            (unsigned)revision, rsdp_addr, oem, rsdt_addr);

    if (!rsdt_addr) {
        pr_info("RSDT address is 0 — assuming 1 CPU\n");
        lapic_ids[0] = 0;
        lapic_base   = 0xFEE00000u;
        ioapic_base  = 0xFEC00000u;
        return 1;
    }

    uint32_t madt_addr = acpi_find_madt(rsdt_addr);
    if (!madt_addr) {
        pr_info("MADT not found — assuming 1 CPU\n");
        lapic_ids[0] = 0;
        lapic_base   = 0xFEE00000u;
        ioapic_base  = 0xFEC00000u;
        return 1;
    }

    acpi_parse_madt(madt_addr);
    pr_info("%d CPU(s) detected\n", cpu_count);
    return cpu_count;
}
