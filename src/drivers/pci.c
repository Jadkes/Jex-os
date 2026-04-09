/**
 * @file pci.c
 * @brief PCI Bus Enumeration and Configuration Implementation.
 * 
 * Implements the low-level communication with the PCI controller
 * via I/O ports 0xCF8 (address) and 0xCFC (data).
 */

#include "pci.h"
#include "ports.h"
#include "terminal.h"
#include "config.h"
#include <stddef.h>

/**
 * @brief Internal list of detected PCI devices.
 * For simplicity, we only store up to 32 devices.
 * In a more advanced OS, this would be a dynamic linked list.
 */
static pci_device_t pci_devices[32];
static int pci_device_count = 0;

/**
 * @brief Read a 32-bit dword from PCI configuration space.
 * 
 * PCI configuration is accessed by writing the address to 0xCF8 (CONFIG_ADDRESS)
 * and then reading the value from 0xCFC (CONFIG_DATA).
 * 
 * Address Format (32-bit):
 * bit 31: Enable bit (must be 1)
 * bits 30-24: Reserved
 * bits 23-16: Bus number
 * bits 15-11: Device (Slot) number
 * bits 10-8: Function number
 * bits 7-2: Register offset
 * bits 1-0: Must be 0
 * 
 * @param bus PCI bus number.
 * @param slot PCI slot (device) number.
 * @param func PCI function number.
 * @param offset Register offset within the config space (aligned to 4 bytes).
 * @return The 32-bit value read from the register.
 */
uint32_t pci_config_read_dword(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset) {
    uint32_t address;
    uint32_t lbus  = (uint32_t)bus;
    uint32_t lslot = (uint32_t)slot;
    uint32_t lfunc = (uint32_t)func;
    uint32_t tmp = 0;

    /* Build the 32-bit address as per the PCI specification */
    address = (uint32_t)((lbus << 16) | (lslot << 11) |
              (lfunc << 8) | (offset & 0xfc) | ((uint32_t)0x80000000));

    /* 1. Write the address to the Address Port (0xCF8) */
    /* We write in bytes for maximum compatibility across different x86 implementations */
    outb(0xCF8 + 3, (address >> 24) & 0xFF);
    outb(0xCF8 + 2, (address >> 16) & 0xFF);
    outb(0xCF8 + 1, (address >> 8) & 0xFF);
    outb(0xCF8 + 0, (address >> 0) & 0xFF);

    /* 2. Read the result from the Data Port (0xCFC) */
    tmp = (uint32_t)inb(0xCFC + 0) | ((uint32_t)inb(0xCFC + 1) << 8) |
          ((uint32_t)inb(0xCFC + 2) << 16) | ((uint32_t)inb(0xCFC + 3) << 24);

    return tmp;
}

/**
 * @brief Write a 32-bit dword to PCI configuration space.
 * 
 * @param bus PCI bus number.
 * @param slot PCI slot (device) number.
 * @param func PCI function number.
 * @param offset Register offset within the config space (aligned to 4 bytes).
 * @param val The 32-bit value to write.
 */
void pci_config_write_dword(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint32_t val) {
    uint32_t address;
    uint32_t lbus  = (uint32_t)bus;
    uint32_t lslot = (uint32_t)slot;
    uint32_t lfunc = (uint32_t)func;

    /* Build the address */
    address = (uint32_t)((lbus << 16) | (lslot << 11) |
              (lfunc << 8) | (offset & 0xfc) | ((uint32_t)0x80000000));

    /* 1. Write the address to the Address Port */
    outb(0xCF8 + 3, (address >> 24) & 0xFF);
    outb(0xCF8 + 2, (address >> 16) & 0xFF);
    outb(0xCF8 + 1, (address >> 8) & 0xFF);
    outb(0xCF8 + 0, (address >> 0) & 0xFF);

    /* 2. Write the data to the Data Port (0xCFC) */
    outb(0xCFC + 3, (val >> 24) & 0xFF);
    outb(0xCFC + 2, (val >> 16) & 0xFF);
    outb(0xCFC + 1, (val >> 8) & 0xFF);
    outb(0xCFC + 0, (val >> 0) & 0xFF);
}

/**
 * @brief Read device information from configuration space and store it.
 * 
 * This probes a specific slot to see if a device is connected.
 * If vendor_id is 0xFFFF, no device is present at this address.
 * 
 * @param bus PCI bus number.
 * @param device PCI device (slot) number.
 */
static void pci_check_device(uint8_t bus, uint8_t device) {
    uint8_t function = 0;

    /* Offset 0: Vendor ID (bits 0-15) and Device ID (bits 16-31) */
    uint32_t reg0 = pci_config_read_dword(bus, device, function, 0);
    uint16_t vendor_id = reg0 & 0xFFFF;
    if (vendor_id == 0xFFFF) return; // Slot is empty

    pci_device_t dev;
    dev.bus = bus;
    dev.device = device;
    dev.function = function;
    dev.vendor_id = vendor_id;
    dev.device_id = (reg0 >> 16) & 0xFFFF;

    /* Offset 8: Revision ID, Prog IF, Subclass, Class Code */
    uint32_t reg8 = pci_config_read_dword(bus, device, function, 8);
    dev.revision_id = reg8 & 0xFF;
    dev.prog_if = (reg8 >> 8) & 0xFF;
    dev.subclass = (reg8 >> 16) & 0xFF;
    dev.class_code = (reg8 >> 24) & 0xFF;

    /* Base Address Registers (BARs) tell us where the device's I/O or Memory is mapped */
    dev.bar0 = pci_config_read_dword(bus, device, function, 0x10);
    dev.bar1 = pci_config_read_dword(bus, device, function, 0x14);
    dev.bar2 = pci_config_read_dword(bus, device, function, 0x18);
    dev.bar3 = pci_config_read_dword(bus, device, function, 0x1C);
    dev.bar4 = pci_config_read_dword(bus, device, function, 0x20);
    dev.bar5 = pci_config_read_dword(bus, device, function, 0x24);

    /* Offset 0x3C: Interrupt Line (bits 0-7) */
    uint32_t reg3C = pci_config_read_dword(bus, device, function, 0x3C);
    dev.irq_line = reg3C & 0xFF;

    /* Add the valid device to our system-wide list */
    if (pci_device_count < 32) {
        pci_devices[pci_device_count++] = dev;
    }
}

/**
 * @brief Initialize PCI subsystem and scan the bus.
 * Iterates through all possible bus/device combinations and populates an internal table.
 */
void init_pci() {
    /* Scan all buses and slots defined in config.h */
    for (uint16_t bus = 0; bus < PCI_MAX_BUS; bus++) {
        for (uint8_t device = 0; device < PCI_MAX_DEVICE; device++) {
            pci_check_device((uint8_t)bus, device);
        }
    }
}

/**
 * @brief Find a PCI device by vendor and device ID in our internal list.
 * 
 * @param vendor_id Manufacturer ID to search for.
 * @param device_id Product ID to search for.
 * @param dev Pointer to a pci_device_t structure to fill with device info if found.
 * @return 1 if found, 0 otherwise.
 */
int pci_find_device(uint16_t vendor_id, uint16_t device_id, pci_device_t* dev) {
    for (int i = 0; i < pci_device_count; i++) {
        if (pci_devices[i].vendor_id == vendor_id && pci_devices[i].device_id == device_id) {
            if (dev) *dev = pci_devices[i];
            return 1;
        }
    }
    return 0;
}
