/**
 * @file power.c
 * @brief Power management implementation.
 *
 * Handles system reboot and shutdown using hardware-specific commands.
 */

#include "power.h"
#include "ports.h"

/**
 * @brief Pulse the CPU reset line via the keyboard controller (PS/2).
 */
void reboot() {
    /* Sending 0xFE to the 8042 status register triggers a CPU reset. */
    outb(0x64, 0xFE);
    
    /* Wait for the reset to happen */
    while(1) {
        __asm__ volatile("hlt");
    }
}

/**
 * @brief Power off the system.
 * 
 * Works primarily for emulators (QEMU, Bochs, VirtualBox) using 
 * non-standard I/O ports. Real hardware would require ACPI.
 */
void shutdown() {
    /* QEMU/Bochs poweroff (ACPI sleep state) */
    outw(0x604, 0x2000);
    
    /* VirtualBox / older QEMU */
    outw(0xB004, 0x2000);
    
    /* If shutdown failed, halt the CPU */
    while(1) {
        __asm__ volatile("hlt");
    }
}
