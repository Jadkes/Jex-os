#include "power.h"
#include "ports.h"

void reboot() {
    /* The standard way to reboot an x86 PC is via the keyboard controller.
       Sending 0xFE to port 0x64 tells the controller to pulse the reset line. */
    outb(0x64, 0xFE);
    
    /* If that fails (it shouldn't), we loop */
    while(1) {
        __asm__ volatile("hlt");
    }
}

void shutdown() {
    /* Shutdown is tricky on real hardware without ACPI, but for QEMU/Bochs
       we can use special debug/power ports. */
       
    // QEMU/Bochs poweroff (16-bit)
    outw(0x604, 0x2000);
    
    // VirtualBox/older QEMU (16-bit)
    outw(0xB004, 0x2000);
    
    /* If we are still here, we halt */
    while(1) {
        __asm__ volatile("hlt");
    }
}
