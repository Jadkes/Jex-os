/**
 * @file pci.h
 * @brief PCI Bus Enumeration and Configuration.
 *
 * Provides functions to scan the PCI bus and read/write configuration space.
 */

#ifndef PCI_H
#define PCI_H

#include <stdint.h>

/* PCI Configuration Ports */
#define PCI_CONFIG_ADDRESS 0xCF8
#define PCI_CONFIG_DATA    0xCFC

/**
 * @brief PCI Device Structure.
 * Holds information about a detected device on the PCI bus.
 */
typedef struct {
    uint8_t  bus;           /**< Bus number (0-255) */
    uint8_t  device;        /**< Device slot (0-31) */
    uint8_t  function;      /**< Function number (0-7) */
    uint16_t vendor_id;     /**< Manufacturer ID (e.g., 0x10EC for Realtek) */
    uint16_t device_id;     /**< Product ID (e.g., 0x8139 for RTL8139) */
    uint8_t  class_code;    /**< General device type (e.g., 0x02 for Network) */
    uint8_t  subclass;      /**< Specific device type (e.g., 0x00 for Ethernet) */
    uint8_t  prog_if;       /**< Programming interface */
    uint8_t  revision_id;   /**< Chip revision ID */
    uint32_t bar0;          /**< Base Address Register 0 */
    uint32_t bar1;          /**< Base Address Register 1 */
    uint32_t bar2;          /**< Base Address Register 2 */
    uint32_t bar3;          /**< Base Address Register 3 */
    uint32_t bar4;          /**< Base Address Register 4 */
    uint32_t bar5;          /**< Base Address Register 5 */
    uint8_t  irq_line;      /**< IRQ number (0-15) */
} pci_device_t;

struct pci_device_id {
    uint16_t vendor;
    uint16_t device;
    uint32_t driver_data;
};

struct pci_driver {
    const char*                 name;
    const struct pci_device_id* id_table;
    int  (*probe)(pci_device_t* dev);
    void (*remove)(pci_device_t* dev);
    struct pci_driver* next;
};

/**
 * @brief Read a 16-bit word from PCI config space.
 *
 * @param bus PCI bus number.
 * @param slot PCI slot (device) number.
 * @param func PCI function number.
 * @param offset Register offset within the config space (byte-aligned).
 * @return The 16-bit value read from the register.
 */
uint16_t pci_config_read_word(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset);

/**
 * @brief Register a PCI driver for automatic bus scanning.
 *
 * When a driver is registered, all PCI buses are scanned for matching
 * device IDs. For each match, the driver's probe function is called
 * with the device information.
 *
 * @param drv Pointer to a statically-allocated pci_driver structure.
 */
void pci_register_driver(struct pci_driver* drv);

/**
 * @brief Read a 32-bit dword from PCI configuration space.
 * 
 * @param bus PCI bus number.
 * @param slot PCI slot (device) number.
 * @param func PCI function number.
 * @param offset Register offset within the config space (aligned to 4 bytes).
 * @return The 32-bit value read from the register.
 */
uint32_t pci_config_read_dword(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset);

/**
 * @brief Write a 32-bit dword to PCI configuration space.
 * 
 * @param bus PCI bus number.
 * @param slot PCI slot (device) number.
 * @param func PCI function number.
 * @param offset Register offset within the config space (aligned to 4 bytes).
 * @param val The 32-bit value to write.
 * @return void
 */
void pci_config_write_dword(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint32_t val);

/**
 * @brief Initialize PCI subsystem and scan for devices.
 * Iterates through all possible bus/device combinations and populates an internal table.
 * 
 * @return void
 */
void init_pci();

/**
 * @brief Find a PCI device by vendor and device ID.
 * 
 * @param vendor_id Manufacturer ID to search for.
 * @param device_id Product ID to search for.
 * @param dev Pointer to a pci_device_t structure to fill with device info if found.
 * @return 1 if found, 0 otherwise.
 */
int pci_find_device(uint16_t vendor_id, uint16_t device_id, pci_device_t* dev);

#endif // PCI_H
