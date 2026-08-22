/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * acpi.h - ACPI table parsing (RSDP, RSDT, MADT)
 *
 * Purpose: Scan BIOS memory for the RSDP, walk the RSDT to find the MADT,
 *          then parse the MADT for local APIC and I/O APIC entries.  Fills
 *          globals that the SMP subsystem uses to enumerate and wake APs.
 *
 * Thread-safety: Called once during boot; not re-entrant.
 */

#ifndef ACPI_H
#define ACPI_H

#include <stdint.h>

/* Max physical CPUs we can handle */
#define MAX_CPUS  4

/* ── ACPI table structures ────────────────────────────── */

/*
 * RSDP (Root System Description Pointer)
 * Found by scanning 0xE0000-0xFFFFF at 16-byte alignment for "RSD PTR ".
 */
typedef struct {
    char     signature[8];      /* "RSD PTR " */
    uint8_t  checksum;
    char     oem_id[6];
    uint8_t  revision;
    uint32_t rsdt_addr;         /* physical address of the RSDT */
} __attribute__((packed)) acpi_rsdp_t;

/*
 * SDT header — common prefix for RSDT, MADT, and other ACPI tables.
 */
typedef struct {
    char     signature[4];
    uint32_t length;
    uint8_t  revision;
    uint8_t  checksum;
    char     oem_id[6];
    char     oem_table_id[8];
    uint32_t oem_revision;
    uint32_t creator_id;
    uint32_t creator_revision;
} __attribute__((packed)) acpi_sdt_t;

/*
 * RSDT — array of 32-bit physical table pointers follows the SDT header.
 */
typedef struct {
    acpi_sdt_t header;
    uint32_t   entry_ptrs[];   /* physical addresses of other SDTs */
} __attribute__((packed)) acpi_rsdt_t;

/*
 * MADT (Multiple APIC Description Table) header.
 * APIC entries follow the header — each has a type/length prefix.
 */
typedef struct {
    acpi_sdt_t header;
    uint32_t   local_apic_addr;
    uint32_t   flags;           /* bit 0 = dual-8259 legacy compat */
} __attribute__((packed)) acpi_madt_t;

/* MADT entry types */
#define MADT_TYPE_LAPIC     0   /* Local APIC override / processor */
#define MADT_TYPE_IOAPIC    1   /* I/O APIC */
#define MADT_TYPE_ISO       2   /* Interrupt Source Override */

/*
 * Generic MADT entry header — every entry starts with type + length.
 */
typedef struct {
    uint8_t type;
    uint8_t length;
} __attribute__((packed)) acpi_madt_entry_t;

/* Local APIC entry (type 0) */
typedef struct {
    acpi_madt_entry_t header;
    uint8_t  acpi_processor_id;
    uint8_t  apic_id;
    uint32_t flags;             /* bit 0 = enabled */
} __attribute__((packed)) acpi_madt_lapic_t;

/* I/O APIC entry (type 1) */
typedef struct {
    acpi_madt_entry_t header;
    uint8_t  ioapic_id;
    uint8_t  reserved;
    uint32_t ioapic_addr;
    uint32_t gsi_base;
} __attribute__((packed)) acpi_madt_ioapic_t;

/* ── Exported globals ─────────────────────────────────── */

extern int     cpu_count;       /* number of enabled CPUs found */
extern uint32_t lapic_base;     /* local APIC MMIO base (default 0xFEE00000) */
extern uint32_t ioapic_base;    /* I/O APIC MMIO base (default 0xFEC00000) */
extern uint32_t lapic_ids[MAX_CPUS];  /* LAPIC ID per logical CPU */

/* ── API ──────────────────────────────────────────────── */

/*
 * acpi_init - Scan BIOS area for RSDP, parse MADT, fill globals.
 *
 * Scans physical memory 0xE0000-0xFFFFF for the RSDP signature, walks
 * the RSDT to locate the MADT, then parses LAPIC and I/O APIC entries.
 * Populates cpu_count, lapic_base, ioapic_base, and lapic_ids[].
 *
 * Return: number of CPUs found (at least 1 for the BSP).
 */
int acpi_init(void);

#endif /* ACPI_H */
