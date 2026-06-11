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
#include "kernel/lockdep.h"
#include "kernel/vsnprintf.h"
#include "devtmpfs.h"
#include "jexfs.h"

// Kernel stack for user mode transitions
uint32_t kernel_stack_top;

// Globals for initcall parameter passing (e.g. pmm_init needs mboot info)
uint32_t g_mboot_magic;
multiboot_info_t* g_mboot_info;

/**
 * @brief Display boot banner with version, build date, RAM, NIC, and FS info.
 */
static void print_banner(void)
{
    char buf[128];

    terminal_writestring("\n");
    terminal_writestring("JexOS v0.1 \x97 i386 \x97 Monolithic\n");
    terminal_writestring("Build: " __DATE__ " " __TIME__ "\n");

    /* RAM size from BIOS int 0x12 */
    uint16_t ram_kb = 0;
    __asm__ volatile("int $0x12" : "=a"(ram_kb) : "a"(0x1200) : "ebx", "ecx", "edx");
    snprintf(buf, sizeof(buf), "RAM: %u KB\n", (uint32_t)ram_kb);
    terminal_writestring(buf);

    /* RTL8139 MAC address — check if driver is initialized */
    if (rtl8139_is_initialized()) {
        uint8_t mac[6];
        rtl8139_get_mac(mac);
        terminal_writestring("NIC: RTL8139 (");
        for (int i = 0; i < 6; i++) {
            snprintf(buf, 3, "%02x", mac[i]);
            terminal_writestring(buf);
            if (i < 5)
                terminal_putchar(':');
        }
        terminal_writestring(")\n");
    }

    terminal_writestring("FS: JexFS v1, 1.44 MB\n");
    terminal_writestring("\n");
}

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

    /* Initialize lock dependency validator */
    lockdep_init();

    /* Initialize devtmpfs and mount /sys */
    devtmpfs_init();
    fs_mount("/sys", "devtmpfs");

    /* Create /home directory */
    {
        if (jexfs_open("/home") < 0) {
            jexfs_mkdir("home");
            pr_info("Created /home\n");
        }

        int home_inode = jexfs_open("/home");
        if (home_inode > 0) {
            cwd_inode = (uint32_t)home_inode;

            if (jexfs_open("user") < 0) {
                jexfs_mkdir("user");
                pr_info("Created /home/user\n");
            }

            /* Set CWD to /home/user */
            int user_inode = jexfs_open("user");
            if (user_inode > 0)
                cwd_inode = (uint32_t)user_inode;
        }
    }

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

    /* 13. Print boot banner then Enter the Shell. */
    print_banner();
    shell_main();
    
    /* 14. Shutdown/Halt. */
    pr_info("Kernel Halted.\n");
    while(1) { __asm__ volatile("hlt"); }
}
