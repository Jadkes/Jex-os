/**
 * @file power.c
 * @brief Power management implementation.
 *
 * Handles system reboot and shutdown using hardware-specific commands.
 */

#include "power.h"
#include "ports.h"

/*
 * reboot - pulse the CPU reset line via the 8042 keyboard controller
 */
void reboot() {
    outb(I8042_STATUS_PORT, I8042_CMD_RESET_CPU);

    while(1) {
        __asm__ volatile("hlt");
    }
}

/* ACPI PM1a sleep value: SLP_TYP=0 with SLP_EN set; both emulator ports
 * accept the same encoding for S5. */
#define PM1A_SLP_EN          0x2000

/*
 * shutdown - request an ACPI poweroff from the emulator's PM1a register.
 * Falls back to halting if the write is ignored (real hardware needs a
 * full ACPI implementation).
 */
void shutdown() {
    outw(QEMU_PM1A_CNT_PORT, PM1A_SLP_EN);

    /* VirtualBox / older QEMU */
    outw(BOCHS_PM1A_CNT_PORT, PM1A_SLP_EN);

    while(1) {
        __asm__ volatile("hlt");
    }
}
