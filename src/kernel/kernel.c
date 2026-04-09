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
    log_serial("JexOS Kernel Started\n");

    /**
     * @brief 3. CPU & Interrupt Setup.
     * GDT: Defines memory segments (Kernel/User Code/Data).
     * IDT: Points to handlers for exceptions and interrupts.
     * ISR: Installs handlers for CPU exceptions (Page Fault, Divide by Zero, etc.).
     * IRQ: Installs handlers for hardware interrupts (Timer, Keyboard, etc.).
     */
    init_gdt();
    init_idt();
    isr_install();
    init_irq();

    /* 4. Basic Hardware Drivers */
    init_keyboard();

    /* 5. Physical Memory Management.
     * Uses the Multiboot memory map to identify available RAM blocks.
     */
    if (magic == MULTIBOOT_MAGIC_VALID) {
        pmm_init(mboot_info);
    } else {
        terminal_writestring("Error: Invalid Multiboot Magic Number!\n");
    }

    /**
     * @brief 6. Virtual Memory & Heap.
     * Paging: Enables 4KB page mapping and memory protection.
     * Heap: Enables dynamic memory allocation (kmalloc/kfree).
     */
    init_paging();
    init_kheap(KERNEL_HEAP_START); // Start heap at 16MB mark

    /**
     * @brief 7. PCI & Networking.
     * init_pci() scans the bus for hardware.
     * init_rtl8139() configures the network card if found.
     * These must be called AFTER kheap/pmm are ready.
     */
    init_pci();
    init_rtl8139();

    /* 8. Filesystem Subsystems.
     * Initializes the VFS and registers filesystem drivers.
     */
    init_fat12();
    fs_init();
    
    /* 9. Multitasking Subsystem.
     * Sets up the task structures and the initial kernel process.
     */
    init_tasking();

    /**
     * @brief 10. System Timer.
     * Configures IRQ0 to fire at 100Hz.
     * This drives preemptive multitasking (context switching).
     */
    init_timer(100);
    
    /* 11. User-mode stack setup. */
    void* kernel_stack = kmalloc(KERNEL_STACK_SIZE);
    kernel_stack_top = (uint32_t)kernel_stack + KERNEL_STACK_SIZE;
    
    /* 12. System Calls & Global Interrupt Enable. */
    init_syscalls();
    
    /* Enable interrupts globally (STI instruction) */
    __asm__ volatile("sti"); 
    
    log_serial("Initialization complete. Launching Shell.\n");

    /* 13. Enter the Shell.
     * This is the main user interface for the kernel.
     */
    shell_main();
    
    /* 14. Shutdown/Halt.
     * If the shell exits, we stop the CPU to prevent undefined behavior.
     */
    terminal_writestring("Kernel Halted.\n");
    while(1) {
        __asm__ volatile("hlt");
    }
}
