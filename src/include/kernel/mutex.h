/*
 * mutex.h - Sleeping mutual exclusion for task context
 *
 * Purpose: Spinlocks are the wrong tool when the critical section
 *          sleeps, does I/O, or runs long (IDE transfers): the holder
 *          burns a core and IRQ-save locking makes ISRs stall behind
 *          it.  A mutex blocks the loser instead -- it parks on the
 *          mutex's FIFO wait list and yields, so no CPU spins.
 *
 * Thread-safety: Task context ONLY.  Never take or release from an
 *          ISR; ISR-side handshakes belong on a completion_t.  The
 *          fast path is a single atomic exchange; contention costs one
 *          scheduler round-trip (wakeup latency is up to one tick).
 */

#ifndef MUTEX_H
#define MUTEX_H

#include "spinlock.h"

struct task;

#include "kernel/lockdep.h"

typedef struct mutex {
    int held;                 /* fast-path flag: 1 = owned */
    struct task* wait_head;   /* FIFO of parked tasks */
    struct task* wait_tail;
    spinlock_t lock;          /* guards held + the wait list */
    const char* name;         /* diagnostics only */
#ifdef JEXOS_DEBUG
    struct lockdep_lock dep;  /* ordering validation (debug flavor) */
#endif
} mutex_t;

/**
 * mutex_init - Prepare a mutex for use
 * @m: mutex
 * @name: static string for debug output (may be NULL)
 */
void mutex_init(mutex_t* m, const char* name);

/**
 * mutex_lock - Take the mutex, blocking until it is available
 * @m: mutex
 *
 * May sleep.  Re-acquires in a loop after each wakeup, so spurious or
 * raced wakeups are safe.
 */
void mutex_lock(mutex_t* m);

/**
 * mutex_unlock - Release the mutex and hand it to the first waiter
 * @m: mutex
 *
 * Ownership passes directly to a woken waiter without dropping through
 * unlocked, which avoids thundering-herd re-fights.
 */
void mutex_unlock(mutex_t* m);

#endif /* MUTEX_H */
