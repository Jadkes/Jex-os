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
#include "test_suite.h"
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
#include "smp.h"
#include "apic.h"

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
    /* Show CPU count (updated by smp_init) */
    char cpuinfo[64];
    snprintf(cpuinfo, sizeof(cpuinfo), "JexOS v0.7.9 \x97 i386 \x97 %d CPU%s\n",
             cpu_count, cpu_count > 1 ? "s" : "");
    terminal_writestring(cpuinfo);
    terminal_writestring("Build: " __DATE__ " " __TIME__ "\n");
    log_serial("JexOS v0.7.9\n");

    /* RAM size from Multiboot info (safe in protected mode) */
    uint32_t ram_kb = g_mboot_info->mem_lower + g_mboot_info->mem_upper;
    snprintf(buf, sizeof(buf), "RAM: %u KB\n", ram_kb);
    terminal_writestring(buf);

    /* Serial mirror of the banner facts; VGA stays as before */
    pr_serial("sys: RAM %u KB\n", ram_kb);

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

        pr_serial("sys: eth0 RTL8139 %02x:%02x:%02x:%02x:%02x:%02x ip %u.%u.%u.%u\n",
                  mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
                  our_ip & 0xFF, (our_ip >> 8) & 0xFF,
                  (our_ip >> 16) & 0xFF, (our_ip >> 24) & 0xFF);
    }

    terminal_writestring("FS: JexFS v2, 4 MB (RAM rootfs)\n");
    pr_serial("sys: fs JexFS v2 rootfs, shell starting\n");
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
	/* VGA console first so later failures are visible */
	terminal_initialize();

    /* Serial log capture via QEMU -serial stdio */
    init_serial();
    klog_init();
    pr_info("JexOS Kernel Started\n");

    /* Store multiboot info globally for initcalls that need it (pmm_init) */
    g_mboot_magic = magic;
    g_mboot_info = mboot_info;

    /* Serial-only boot diagnostics (VGA output stays unchanged) */
    pr_serial("boot: multiboot magic=0x%08x flags=0x%08x\n",
              magic, mboot_info->flags);

    /* Initcalls in section order:
     *   early_initcalls:  CPU setup, interrupt controllers, memory management
     *   device_initcalls: drivers, filesystems, higher-level subsystems */
    initcalls_run();

    lockdep_init();

    devtmpfs_init();
    fs_mount("/sys", "devtmpfs");

    pr_serial("mem: %u/%u MB used, %u KB free\n",
              pmm_get_used_memory() >> 20, pmm_get_total_memory() >> 20,
              pmm_get_free_memory() >> 10);

    /* CWD starts at root */
    cwd_inode = 1;

    /* SMP boot: scan ACPI, init APIC, wake APs. Runs before interrupts
     * are enabled and before the timer is initialized. */
    smp_init();

    /* Timer needs an explicit frequency; kept out of initcalls */
    pr_info("Initializing Timer...\n");
    init_timer(100);

    /* Calibrate the LAPIC timer against the now-running PIT, arm the
     * BSP's and signal the APs (waiting in their idle loops) to arm
     * theirs.  Preemption is per-CPU from here on. */
    init_lapic_timer();
    smp_timers_go();


    /* Kernel stack for the initial task */
    pr_info("Setup Stack...\n");
    void* kernel_stack = kmalloc(KERNEL_STACK_SIZE);
    kernel_stack_top = (uint32_t)kernel_stack + KERNEL_STACK_SIZE;

    /* Do NOT overwrite cur_task()->kstack here — it was already set to
     * the real kernel stack page by alloc_kernel_stack() in init_tasking().
     * Overwriting it with the heap address would make set_kernel_stack()
     * point TSS.esp0 at heap memory instead of the mapped stack page. */

    /* Enable interrupts globally (STI instruction) */
    __asm__ volatile("sti");

    /* First boot line proving the clock actually runs at 100 Hz. */
    timer_selftest();

    /* Sampled late enough that every AP has spun out of its park loop
     * and armed its own timer -- an immediate read raced them. */
    int online_cpus = 0;
    for (int i = 0; i < cpu_count; i++)
        if (cpus[i].online || cpus[i].is_bsp)
            online_cpus++;
    pr_serial("smp: %d/%d CPUs online, lapic timers armed=%d\n",
              online_cpus, cpu_count, lapic_timer_armed_count());

    pr_serial("task: pid %d on cpu%d, kstack top 0x%08x\n",
              cur_task() ? cur_task()->id : 0, cpu_id(), kernel_stack_top);

#ifdef JEXOS_SELFTEST
    pr_info("SELFTEST build: running full kernel self-test suite\n");
    {
        int failures = run_selftests();
        pr_serial("selftest: done, failures=%d\n", failures);
        /*
         * isa-debug-exit protocol: a byte written to port 0xf4 makes
         * QEMU exit with code (value << 1) | 1.
         * Pass uses 0x10 -> exit 33, deliberately far from QEMU's own
         * argument-error exit of 1 so an early crash can never look
         * like a green run.  Failures use 3 -> exit 7.
         */
        outb(0xf4, failures ? 3 : 0x10);
        for (;;)
            __asm__ volatile("hlt");
    }
#endif

    pr_info("Starting Shell...\n");

    print_banner();
    shell_main();

    /* Not reached unless the shell exits */
    pr_info("Kernel Halted.\n");
    while(1) { __asm__ volatile("hlt"); }
}
