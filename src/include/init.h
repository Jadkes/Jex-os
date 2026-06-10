/**
 * @file init.h
 * @brief Initcall framework — automatic driver initialization via linker sections.
 *
 * Purpose: Drivers register their init functions using early_init() or
 *          device_init() macros. Function pointers are collected into the
 *          .early_initcalls and .device_initcalls linker sections. At boot,
 *          initcalls_run() iterates both sections in order, replacing the
 *          manual list of calls in kernel_main().
 *
 * Design:
 *   - early_init: CPU setup, interrupt controllers, memory management.
 *     (GDT, IDT, ISR, IRQ, keyboard, PMM, paging, heap)
 *   - device_init: Drivers, filesystems, subsystems that need MM/CPU ready.
 *     (PCI, network, FS, kallsyms, tasking, syscalls)
 *
 * Thread-safety: Called once during boot, no concurrency.
 */

#ifndef INIT_H
#define INIT_H

/* Initcall function type — all init functions share this signature */
typedef void (*initcall_t)(void);

/*
 * Place a function pointer into one of two linker-collected sections.
 *
 * early_init:  used for CPU / interrupt / memory init (runs first).
 * device_init: used for drivers and subsystems (runs after memory is up).
 *
 * Each macro declares a static pointer variable stored in the appropriate
 * section.  The linker aggregates all such pointers, and initcalls_run()
 * iterates them in link order.
 */
#define early_init(fn)  static initcall_t __initcall_early_##fn \
    __attribute__((used, section(".early_initcalls"))) = fn

#define device_init(fn) static initcall_t __initcall_##fn \
    __attribute__((used, section(".device_initcalls"))) = fn

/**
 * initcalls_run - Execute all registered initcalls in section order.
 *
 * Runs early_initcalls first (CPU, memory), then device_initcalls
 * (drivers, filesystems).  Called once during kernel boot from
 * kernel_main().
 */
void initcalls_run(void);

#endif /* INIT_H */
