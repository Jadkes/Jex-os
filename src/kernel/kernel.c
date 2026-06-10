/**
 * @file kernel.c
 * @brief Main Kernel Entry Point
 *
 * This file contains the main entry point for the JexOS kernel.
 * It initializes core subsystems (memory, interrupts, drivers, filesystem)
 * and hands over control to the shell.
 */

#define pr_fmt(fmt) "[KERNEL] " fmt
#include "kernel/printk.h"

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
#include "klog.h"
#include "syscall.h"
#include "pci.h"
#include "rtl8139.h"
#include "net.h"
#include "kernel/kallsyms.h"

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
    klog_init();
    pr_info("JexOS Kernel Started\n");

    /**
     * @brief 3. CPU & Interrupt Setup.
     * GDT: Defines memory segments (Kernel/User Code/Data).
     * IDT: Points to handlers for exceptions and interrupts.
     * ISR: Installs handlers for CPU exceptions (Page Fault, Divide by Zero, etc.).
     * IRQ: Installs handlers for hardware interrupts (Timer, Keyboard, etc.).
     */
    pr_info("Init GDT...\n");
    init_gdt();
    pr_info("Init IDT...\n");
    init_idt();
    pr_info("Init ISR...\n");
    isr_install();
    pr_info("Init IRQ...\n");
    init_irq();

    /* 4. Basic Hardware Drivers */
    pr_info("Init Keyboard...\n");
    init_keyboard();

    /* 5. Physical Memory Management.
     * Uses the Multiboot memory map to identify available RAM blocks.
     */
    pr_info("Init PMM...\n");
    if (magic == MULTIBOOT_MAGIC_VALID) {
        pmm_init(mboot_info);
    } else {
        pr_err("Invalid Multiboot Magic!\n");
    }

    /**
     * @brief 6. Virtual Memory & Heap.
     * Paging: Enables 4KB page mapping and memory protection.
     * Heap: Enables dynamic memory allocation (kmalloc/kfree).
     */
    pr_info("Init Paging...\n");
    init_paging();
    pr_info("Init Heap...\n");
    init_kheap(KERNEL_HEAP_START);

    /**
     * @brief 7. PCI & Networking.
     * Disabled for now - focus on basic kernel boot
     */
    pr_info("Init PCI...\n");
    init_pci();
    pr_info("Init RTL8139...\n");
    init_rtl8139();
    pr_info("Init Net Stack...\n");
    net_init();

    /* 8. Filesystem Subsystems. */
    pr_info("Init FAT12...\n");
    init_fat12();
    pr_info("Init VFS...\n");
    fs_init();

    /* 9. Kallsyms: Initialize symbol table for backtrace resolution. */
    pr_info("Init Kallsyms...\n");
    kallsyms_init();

    /* 10. Multitasking Subsystem. */
    pr_info("Init Tasking...\n");
    init_tasking();

    /* 10. System Timer. */
    pr_info("Init Timer...\n");
    init_timer(100);
    
    /* 11. User-mode stack setup. */
    pr_info("Setup Stack...\n");
    void* kernel_stack = kmalloc(KERNEL_STACK_SIZE);
    kernel_stack_top = (uint32_t)kernel_stack + KERNEL_STACK_SIZE;
    
    /* 12. System Calls & Global Interrupt Enable. */
    pr_info("Init Syscalls...\n");
    init_syscalls();
    
    /* Enable interrupts globally (STI instruction) */
    __asm__ volatile("sti"); 
    
    pr_info("Starting Shell...\n");

    /* 13. Enter the Shell. */
    shell_main();
    
    /* 14. Shutdown/Halt. */
    pr_info("Kernel Halted.\n");
    while(1) { __asm__ volatile("hlt"); }
}
