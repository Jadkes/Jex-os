/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * smp.h - Symmetric Multi-Processing structures
 *
 * Purpose: Defines the per-CPU data structure (cpu_t), the IPC mailbox
 *          for cross-core messaging, and the SMP boot/management API.
 *
 * Design: Terry A. Davis approach — each CPU has its own ready queue,
 *         page directory, and mailbox.  Tasks are assigned to a CPU at
 *         creation time and never migrate.  Cross-CPU communication
 *         happens through the mailbox (no spinlocks).
 *
 * Thread-safety: cpu_t fields are only accessed by the owning CPU (except
 *                the mailbox, which uses atomic read/write of head/tail).
 */

#ifndef SMP_H
#define SMP_H

#include <stdint.h>
#define NR_CPUS       4
#define MAILBOX_SIZE  16

/* Forward declaration — task_t is defined in task.h */
struct task;

/* ── IPC Mailbox ─────────────────────────────────────── */

typedef struct {
    uint32_t type;      /* 0=empty, 1=spawn, 2=kill, 3=data */
    uint32_t arg0;
    uint32_t arg1;
} mailbox_slot_t;

typedef struct {
    uint32_t       head;    /* written by SENDER */
    uint32_t       tail;    /* written by RECEIVER (the owning CPU) */
    mailbox_slot_t slots[MAILBOX_SIZE];
} mailbox_t;

/* ── Per-CPU structure ────────────────────────────────── */

typedef struct cpu {
    uint32_t        lapic_id;
    int             is_bsp;
    volatile int    online;          /* set by ap_startup, polled by BSP */
    struct task*    ready_queue;
    struct task*    current_task;
    uint32_t        kstack_top;      /* TSS.ESP0 for this CPU */
    void*           page_directory;  /* pointer to this CPU's page dir */
    uint32_t        apic_timer_ticks;
    mailbox_t       mailbox;
    int             cpu_number;      /* index into cpus[] */
    struct task*    prev_run;        /* last task switched away from; keeps
                                         its scheduler claim until the next
                                         switch so no other CPU can resume
                                         it while we unwind off its stack */
} cpu_t;

/* ── Globals ──────────────────────────────────────────── */

extern cpu_t cpus[NR_CPUS];
extern int   cpu_count;

/* ── Inline helpers ───────────────────────────────────── */

/* Return the logical index of the current CPU (0 for BSP). */
int cpu_id(void);

static inline cpu_t* this_cpu(void) { return &cpus[cpu_id()]; }

/* ── Public API ───────────────────────────────────────── */

/* smp_init - Boot all application processors (APs). */
void smp_init(void);

/* ap_startup - C entry point called by the trampoline on each AP. */
void ap_startup(void);

/* ap_scheduler_loop - Per-CPU idle/scheduler loop for APs. */
void ap_scheduler_loop(void);

/* smp_timers_go - Let parked APs arm their LAPIC timers (BSP call,
 * after init_lapic_timer()). */
void smp_timers_go(void);

/* Mailbox API */
int  mailbox_send(int target_cpu, mailbox_slot_t* msg);
int  mailbox_recv(mailbox_t* mb, mailbox_slot_t* msg);

#endif /* SMP_H */
