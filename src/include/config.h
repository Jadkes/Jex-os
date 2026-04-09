#ifndef CONFIG_H
#define CONFIG_H

/**
 * @file config.h
 * @brief Global configuration constants for JexOS.
 *
 * Centralizes magic numbers and system parameters to avoid hardcoding.
 */

/* Memory Management Configuration */
#define KERNEL_HEAP_START       0x1000000      // Start of kernel heap (16MB mark)
#define KERNEL_HEAP_SIZE        (1024 * 1024)  // 1MB initial heap size
#define KERNEL_STACK_SIZE       8192           // 8KB kernel stack

/* Hardware I/O Ports */
#define SERIAL_COM1_BASE        0x3F8          // COM1 Base Port
#define VGA_CTRL_PORT           0x3D4          // VGA Controller Index Register
#define VGA_DATA_PORT           0x3D5          // VGA Controller Data Register

/* Video Memory */
#define VGA_MEMORY_ADDR         0xB8000        // Physical address of VGA text buffer
#define VGA_WIDTH               80             // Terminal width in characters
#define VGA_HEIGHT              25             // Terminal height in characters

/* Multiboot Specification */
#define MULTIBOOT_MAGIC_VALID   0x2BADB002     // Expected EAX value from bootloader

/* System Limits */
#define MAX_OPEN_FILES          32             // Maximum file descriptors per process

/* Network Device IDs */
#define PCI_VENDOR_REALTEK      0x10EC
#define PCI_DEVICE_RTL8139      0x8139

/* PCI Limits */
#define PCI_MAX_BUS             256
#define PCI_MAX_DEVICE          32
#define PCI_MAX_FUNCTION        8

#endif // CONFIG_H
