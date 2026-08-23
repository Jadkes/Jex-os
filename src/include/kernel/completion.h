/*
 * completion.h - Wait for an event that happens in another context
 *
 * Purpose: The task/ISR handshake primitive.  A driver thread starts a
 *          hardware operation, calls completion_wait(); the ISR (or
 *          workqueue) calls completion_signal() when the hardware is
 *          finished.  Unlike a mutex this has no ownership: any
 *          context can signal it, many tasks may wait, and the flag
 *          survives if the signal lands before anyone waits.
 *
 * Semantics: completion_signal() sets the flag and wakes all current
 *          waiters; exactly one of them consumes it.  A waiter that
 *          arrives after the flag was set consumes it immediately
 *          without sleeping.  Reuse by calling completion_reset().
 */

#ifndef COMPLETION_H
#define COMPLETION_H

#include <stdint.h>
#include "spinlock.h"

struct task;

typedef struct completion {
    volatile int done;        /* event pending */
    struct task* wait_head;   /* FIFO of parked tasks */
    struct task* wait_tail;
    spinlock_t lock;
} completion_t;

/**
 * completion_init - Prepare a completion for use
 * @c: completion
 */
void completion_init(completion_t* c);

/**
 * completion_wait - Block until someone signals the completion
 * @c: completion
 *
 * Consumes one pending signal.  May sleep.
 */
void completion_wait(completion_t* c);

/**
 * completion_done - Signal the completion from ISR or task context
 * @c: completion
 *
 * IRQ-safe.  Wakes every parked waiter; the first to run consumes the
 * flag.
 */
void completion_signal(completion_t* c);

/**
 * completion_reset - Clear a consumed/stale flag
 * @c: completion
 *
 * Call between operations when no waiter is parked.
 */
void completion_reset(completion_t* c);

#endif /* COMPLETION_H */
