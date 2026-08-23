/**
 * @file spinlock.h
 * @brief Busy-wait spinlocks for cross-CPU mutual exclusion.
 *
 * Local cli/sti only shields a critical section from interrupts on the
 * SAME cpu — another core walks straight through it.  These locks are the
 * cross-cpu half; hold them around shared structures (PMM bitmap, slab
 * lists, task queues).
 *
 * Rule of thumb: if the section is reachable with IF=1 and can also run
 * in interrupt context on the same cpu, use the irqsave pair — a bare
 * spin_lock() there deadlocks against the interrupted holder.
 */

#ifndef SPINLOCK_H
#define SPINLOCK_H

#include <stdint.h>
#include "ports.h"

typedef struct {
    volatile uint32_t locked;
} spinlock_t;

#define SPINLOCK_INIT { 0 }

static inline void spin_init(spinlock_t* l) { l->locked = 0; }

static inline void spin_lock(spinlock_t* l)
{
    while (__sync_lock_test_and_set(&l->locked, 1)) {
        while (l->locked)
            __asm__ volatile("pause");
    }
}

static inline void spin_unlock(spinlock_t* l)
{
    __sync_lock_release(&l->locked);
}

/* irqsave variants for sections reachable with IF=1 */
static inline void spin_lock_irqsave(spinlock_t* l, uint32_t* flags)
{
    *flags = irq_save();
    spin_lock(l);
}

static inline void spin_unlock_irqrestore(spinlock_t* l, uint32_t flags)
{
    spin_unlock(l);
    irq_restore(flags);
}

#endif // SPINLOCK_H
