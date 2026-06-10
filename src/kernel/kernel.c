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
#include "init.h"
#include "devtmpfs.h"

// Kernel stack for user mode transitions
uint32_t kernel_stack_top;

// Globals for initcall parameter passing (e.g. pmm_init needs mboot info)
uint32_t g_mboot_magic;
multiboot_info_t* g_mboot_info;

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

    /* Store multiboot info globally for initcalls that need it (pmm_init) */
    g_mboot_magic = magic;
    g_mboot_info = mboot_info;

    /* 3-12. Run all registered initcalls in section order:
     *   early_initcalls:  CPU setup, interrupt controllers, memory management
     *   device_initcalls: drivers, filesystems, higher-level subsystems
     */
    initcalls_run();

    /* Initialize devtmpfs and mount /sys */
    devtmpfs_init();
    fs_mount("/sys", "devtmpfs");

    /* 13. System Timer — needs explicit frequency parameter, kept manual */
    pr_info("Init Timer...\n");
    init_timer(100);

    /* 14. User-mode stack setup */
    pr_info("Setup Stack...\n");
    void* kernel_stack = kmalloc(KERNEL_STACK_SIZE);
    kernel_stack_top = (uint32_t)kernel_stack + KERNEL_STACK_SIZE;

    /* Enable interrupts globally (STI instruction) */
    __asm__ volatile("sti"); 
    
    pr_info("Starting Shell...\n");

    /* 13. Enter the Shell. */
    shell_main();
    
    /* 14. Shutdown/Halt. */
    pr_info("Kernel Halted.\n");
    while(1) { __asm__ volatile("hlt"); }
}
