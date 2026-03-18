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

// Global Configuration
#include "config.h"

// Kernel Subsystems
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
	// Initialize the console output (VGA) first to display logs.
	terminal_initialize();

    // Initialize serial port for debugging output (COM1).
    init_serial();
    log_serial("JexOS Kernel Started\n");

    // Initialize CPU descriptor tables and interrupt handling.
    // GDT: Global Descriptor Table (memory segments)
    // IDT: Interrupt Descriptor Table (interrupt handlers)
    // ISR/IRQ: Interrupt Service Routines and Requests
    init_gdt();
    init_idt();
    isr_install();
    init_irq();

    // Initialize Hardware Drivers.
    init_keyboard();

    // Initialize Physical Memory Manager if bootloader magic is valid.
    if (magic == MULTIBOOT_MAGIC_VALID) {
        pmm_init(mboot_info);
    } else {
        terminal_writestring("Error: Invalid Multiboot Magic Number!\n");
        // We might want to panic here, but let's continue cautiously.
    }

    // Initialize Virtual Memory (Paging) and Kernel Heap.
    init_paging();
    init_kheap(KERNEL_HEAP_START); // Start heap at 16MB mark

    // Initialize Filesystem Subsystems.
    // FAT12 is used for the boot disk compatibility (if applicable).
    // JexFS is the native filesystem.
    init_fat12();
    fs_init();
    
    // Initialize Tasking/Multitasking Subsystem.
    init_tasking();

    // Start the System Timer (Preemptive Multitasking Tick).
    // 100 Hz = 10ms timeslice.
    init_timer(100);
    
    // Allocate a separate stack for user-mode transitions.
    // kmalloc is declared in kheap.h
    void* kernel_stack = kmalloc(KERNEL_STACK_SIZE);
    kernel_stack_top = (uint32_t)kernel_stack + KERNEL_STACK_SIZE;
    
    // Initialize System Calls and Enable Interrupts.
    init_syscalls();
    
    __asm__ volatile("sti"); // Enable interrupts (Start The Interrupts)
    
    log_serial("Initialization complete. Launching Shell.\n");

    // Launch the interactive kernel shell.
    // This function should not return during normal operation.
    shell_main();
    
    // If shell returns, halt the CPU loop.
    terminal_writestring("Kernel Halted.\n");
    while(1) {
        __asm__ volatile("hlt");
    }
}
