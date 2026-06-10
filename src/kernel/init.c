/**
 * @file init.c
 * @brief Initcall runner — iterates linker-collected init function pointers.
 *
 * Purpose: Provides the initcalls_run() entry point that walks the
 *          .early_initcalls and .device_initcalls sections, calling each
 *          registered init function.  Replaces the manual list of ~18 calls
 *          in kernel_main() with two automatic loops.
 *
 * Design:
 *   - Section boundaries are defined by the linker (__early_initcall_start,
 *     __early_initcall_end, __device_initcall_start, __device_initcall_end).
 *   - All initcalls within a section run in link order.
 *   - After both sections complete, all subsystems are initialized.
 *
 * Thread-safety: Called once during boot — no concurrency concerns.
 */

#include "init.h"
#include "kernel/printk.h"

/**
 * initcalls_run - Execute all registered initcalls.
 *
 * Iterates early_initcalls (CPU, memory management) first, then
 * device_initcalls (drivers, filesystems, higher-level subsystems).
 */
void initcalls_run(void)
{
    extern initcall_t __early_initcall_start[];
    extern initcall_t __early_initcall_end[];
    extern initcall_t __device_initcall_start[];
    extern initcall_t __device_initcall_end[];

    pr_info("Running early initcalls...\n");
    for (initcall_t* fn = __early_initcall_start; fn < __early_initcall_end; fn++) {
        (*fn)();
    }

    pr_info("Running device initcalls...\n");
    for (initcall_t* fn = __device_initcall_start; fn < __device_initcall_end; fn++) {
        (*fn)();
    }

    pr_info("Initcalls complete\n");
}
