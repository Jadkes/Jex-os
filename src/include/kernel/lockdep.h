/*
 * lockdep.h - Lock Dependency Validator
 *
 * Purpose: Catch lock-order inversions before they deadlock the box.
 *          Validates every sleeping-lock acquisition against a global
 *          table of observed orderings; an acquisition that closes a
 *          cycle warns loudly at acquire time instead of hanging.
 *
 * Design:  Per-task held-lock stacks (keyed by cur_task()) feed a
 *          persistent global edge table keyed by lock NAME.  Names act
 *          as lock classes, like Linux lockdep classes: every mutex
 *          sharing a static name shares ordering rules.  Edges are
 *          recorded once; cycle checks are reachability queries over
 *          the recorded set only.
 *
 * Scope:   Sleeping locks (mutexes) in task context.  Spinlocks are
 *          covered by the raw-uart stuck watchdog in spinlock.h.
 *          Release builds compile hook sites to nothing.
 *
 * Thread-safety: All entry points serialize on an internal irqsave
 *          spinlock; safe from any task context on any cpu.
 */

#ifndef LOCKDEP_H
#define LOCKDEP_H

#include <stdint.h>

/* Per-task held stack limits */
#define LOCKDEP_MAX_HELD   8
#define LOCKDEP_MAX_TASKS  16
/* Global lock-class ordering table */
#define LOCKDEP_MAX_EDGES  64

struct lockdep_lock {
    const char* name;    /* lock class id: must be a static string */
    uint32_t    ip;      /* last acquire site, for reports */
};

/*
 * Master switch: validation runs only while lockdep_enabled is set.
 * It defaults OFF because the always-on wiring destabilizes the
 * self-test suite in a way not yet understood (see TODO.md) -- the
 * shell `lockdep` command flips it live for targeted hunts.
 */
extern int lockdep_enabled;

void lockdep_init(void);

/*
 * lockdep_acquire - validate and record taking @l while holding others
 * @l:  the lock being taken (dep.name must be set)
 * @ip: caller address for diagnostics (__builtin_return_address(0))
 *
 * Warns (and counts in lockdep_warnings) if this acquisition closes an
 * ordering cycle.  The bad edge is NOT recorded, so one warned
 * inversion does not poison future checks.
 */
void lockdep_acquire(struct lockdep_lock* l, uint32_t ip);

/*
 * lockdep_release - drop @l from the current task's held stack
 * Warns if @l was not tracked as held (skewed bookkeeping, e.g. after
 * a warned deadlock path).
 */
void lockdep_release(struct lockdep_lock* l);

/*
 * lockdep_would_deadlock - pure query: would taking @want while
 * holding @held close a cycle?  Records nothing.  Used by
 * lockdep_acquire internally and directly by self-tests.
 *
 * @return 1 if a cycle would be created, 0 otherwise
 */
int lockdep_would_deadlock(struct lockdep_lock* held,
                           struct lockdep_lock* want);

/*
 * lockdep_task_exit - forget a task's held stack
 * Call before the task struct is freed; prevents a recycled task
 * pointer from inheriting a dead task's lock history.
 */
void lockdep_task_exit(void* task);

void lockdep_dump(void);

/* Total warnings (cycles + skewed releases); test/diagnostic hook */
extern int lockdep_warnings;

#endif /* LOCKDEP_H */
