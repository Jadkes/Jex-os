/**
 * @file device.h
 * @brief Generic device registration and enumeration.
 *
 * Purpose: Provide a bus-agnostic device list so upper layers (filesystems,
 *          network stack, shell commands) can iterate over all registered
 *          devices without knowing about PCI, USB, etc.
 *
 * Design: Simple singly-linked list.  Each device carries its name, IRQ,
 *         I/O base, MMIO base, and an opaque driver_data pointer.
 *
 * Thread-safety: Not thread-safe.  Registration happens during boot
 *                (single-threaded).  Iteration from shell is also
 *                single-threaded.
 */

#ifndef DEVICE_H
#define DEVICE_H

#include <stdint.h>

struct device {
    const char* name;        /* Human-readable device name */
    uint32_t    irq;         /* IRQ number */
    uint32_t    io_base;     /* I/O port base address */
    void*       mmio_base;   /* Memory-mapped I/O base */
    void*       driver_data; /* Opaque per-driver data */
    const char* bus_name;    /* Bus type name (e.g. "pci") */
    struct device* next;     /* Next in the linked list */
};

/**
 * device_register - Add a device to the global device list.
 * @dev: Statically-allocated device structure (pre-filled by driver).
 */
void device_register(struct device* dev);

/**
 * device_iter_first - Return the head of the device list.
 */
struct device* device_iter_first(void);

/**
 * device_iter_next - Return the next device after @prev.
 * @prev: The device returned by the previous call to iter_first/iter_next.
 */
struct device* device_iter_next(struct device* prev);

#endif /* DEVICE_H */
