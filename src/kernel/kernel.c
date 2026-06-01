/**
 * @file kernel.c
 * @brief Main Kernel Entry Point
 *
 * This file contains the main entry point for the JexOS kernel.
 * It initializes core subsystems (memory, interrupts, drivers, filesystem)
 * and hands over control to the shell.
 */

// Standard headers
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "config.h"
#include "gdt.h"
#include "idt.h"
#include "isr.h"
#include "irq.h"
#include "keyboard.h"
#include "shell.h"
#include "terminal.h"
#include "serial.h"
#include "multiboot.h"
#include "pmm.h"
#include "paging.h"
#include "ports.h"
#include "kheap.h"
#include "fat12.h"
#include "timer.h"
#include "fs.h"
#include "task.h"
#include "syscall.h"
#include "pci.h"
#include "rtl8139.h"
#include "net.h"

// Kernel stack for user mode transitions
uint32_t kernel_stack_top;

/**
 * @brief Kernel Entry Point
 *
 * Called by the bootloader (boot.s).
 *
 * @param magic The Multiboot magic number (should be MULTIBOOT_MAGIC_VALID).
 * @param mboot_info Pointer to the Multiboot information structure.
 */
void kernel_main(uint32_t magic, multiboot_info_t* mboot_info) {
	/* 1. Initialize the console output (VGA) first to display logs and errors. */
	terminal_initialize();

    /* 2. Initialize serial port for debugging output (COM1).
     * Useful for capturing kernel logs in QEMU via -serial stdio.
     */
    init_serial();
    terminal_writestring("JexOS Kernel Started\n");
    log_serial("JexOS Kernel Started\n");

    /**
     * @brief 3. CPU & Interrupt Setup.
     * GDT: Defines memory segments (Kernel/User Code/Data).
     * IDT: Points to handlers for exceptions and interrupts.
     * ISR: Installs handlers for CPU exceptions (Page Fault, Divide by Zero, etc.).
     * IRQ: Installs handlers for hardware interrupts (Timer, Keyboard, etc.).
     */
    terminal_writestring("Init GDT...\n"); log_serial("Init GDT...\n");
    init_gdt();
    terminal_writestring("Init IDT...\n"); log_serial("Init IDT...\n");
    init_idt();
    terminal_writestring("Init ISR...\n"); log_serial("Init ISR...\n");
    isr_install();
    terminal_writestring("Init IRQ...\n"); log_serial("Init IRQ...\n");
    init_irq();

    /* 4. Basic Hardware Drivers */
    terminal_writestring("Init Keyboard...\n"); log_serial("Init Keyboard...\n");
    init_keyboard();

    /* 5. Physical Memory Management.
     * Uses the Multiboot memory map to identify available RAM blocks.
     */
    terminal_writestring("Init PMM...\n"); log_serial("Init PMM...\n");
    if (magic == MULTIBOOT_MAGIC_VALID) {
        pmm_init(mboot_info);
    } else {
        terminal_writestring("Error: Invalid Multiboot Magic!\n");
    }

    /**
     * @brief 6. Virtual Memory & Heap.
     * Paging: Enables 4KB page mapping and memory protection.
     * Heap: Enables dynamic memory allocation (kmalloc/kfree).
     */
    terminal_writestring("Init Paging...\n"); log_serial("Init Paging...\n");
    init_paging();
    terminal_writestring("Init Heap...\n"); log_serial("Init Heap...\n");
    init_kheap(KERNEL_HEAP_START);

    /**
     * @brief 7. PCI & Networking.
     * Disabled for now - focus on basic kernel boot
     */
    terminal_writestring("Init PCI...\n"); log_serial("Init PCI...\n");
    init_pci();
    terminal_writestring("Init RTL8139...\n"); log_serial("Init RTL8139...\n");
    init_rtl8139();
    terminal_writestring("Init Net Stack...\n"); log_serial("Init Net Stack...\n");
    net_init();

    /* 8. Filesystem Subsystems. */
    terminal_writestring("Init FAT12...\n"); log_serial("Init FAT12...\n");
    init_fat12();
    terminal_writestring("Init VFS...\n"); log_serial("Init VFS...\n");
    fs_init();
    
    /* 9. Multitasking Subsystem. */
    terminal_writestring("Init Tasking...\n"); log_serial("Init Tasking...\n");
    init_tasking();

    /* 10. System Timer. */
    terminal_writestring("Init Timer...\n"); log_serial("Init Timer...\n");
    init_timer(100);
    
    /* 11. User-mode stack setup. */
    terminal_writestring("Setup Stack...\n"); log_serial("Setup Stack...\n");
    void* kernel_stack = kmalloc(KERNEL_STACK_SIZE);
    kernel_stack_top = (uint32_t)kernel_stack + KERNEL_STACK_SIZE;
    
    /* 12. System Calls & Global Interrupt Enable. */
    terminal_writestring("Init Syscalls...\n"); log_serial("Init Syscalls...\n");
    init_syscalls();
    
    /* Enable interrupts globally (STI instruction) */
    __asm__ volatile("sti"); 
    
    terminal_writestring("Starting Shell...\n"); log_serial("Starting Shell...\n");

    /* 13. Enter the Shell. */
    shell_main();
    
    /* 14. Shutdown/Halt. */
    terminal_writestring("Kernel Halted.\n");
    while(1) { __asm__ volatile("hlt"); }
}
