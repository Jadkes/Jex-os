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

/* Debug-flavor watchdog: if a contender spins this long without winning,
 * the lock is almost certainly deadlocked -- scream straight to the UART
 * (raw writes, no console lock, safe from any context) and keep screaming
 * every interval so a live hang stays distinguishable from silence.
 * The printed word is the address of l->locked; match it against
 * `nm build/kernel.elf | grep tasklist_lock` (or whichever lock). */
#ifdef JEXOS_DEBUG
#define SPIN_STUCK_LIMIT (1u << 22)
extern void write_serial(char a);

static inline void spin_stuck_report_raw(const char* tag,
                                         const volatile void* lock_word)
{
    const char* msg = "\n[[ ";
    for (const char* c = msg; *c; c++) write_serial(*c);
    for (const char* c = tag; *c; c++) write_serial(*c);
    const char* mid = " @ 0x";
    for (const char* c = mid; *c; c++) write_serial(*c);
    uintptr_t addr = (uintptr_t)lock_word;
    for (int i = 28; i >= 0; i -= 4) {
        char nibble = (char)((addr >> i) & 0xF);
        write_serial((char)(nibble < 10 ? '0' + nibble : 'a' + nibble - 10));
    }
    const char* tail = " ]]\n";
    for (const char* c = tail; *c; c++) write_serial(*c);
}

static inline void spin_stuck_report(const volatile void* lock_word)
{
    spin_stuck_report_raw("SPINLOCK STUCK", lock_word);
}
/* One-shot trace when a contender finds the lock taken: rare enough on
 * a healthy system to log without flooding, and it names the exact lock
 * word the moment contention begins. */
static inline void spin_contend_once(const volatile void* lock_word)
{
    static volatile uint32_t contend_shown_mask;
    uintptr_t slot = ((uintptr_t)lock_word >> 4) & 31;
    uint32_t bit = 1u << slot;
    if (!(contend_shown_mask & bit)) {
        contend_shown_mask |= bit;
        spin_stuck_report_raw("SPINCONTEND", lock_word);
    }
}
#else
#define SPIN_STUCK_LIMIT 0
static inline void spin_stuck_report(const volatile void* lock_word)
{
    (void)lock_word;
}
#endif

static inline void spin_init(spinlock_t* l) { l->locked = 0; }

static inline void spin_lock(spinlock_t* l)
{
    while (__sync_lock_test_and_set(&l->locked, 1)) {
        uint32_t spins = 0;
#ifdef JEXOS_DEBUG
        spin_contend_once(l);
#endif
        while (l->locked) {
            __asm__ volatile("pause");
            if (SPIN_STUCK_LIMIT && ++spins >= SPIN_STUCK_LIMIT) {
                spin_stuck_report(&l->locked);
                spins = 0;
            }
        }
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
