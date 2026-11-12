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
 *
 * Debug flavors stamp each acquire with __FILE__/__LINE__; the
 * stuck-reporter prints the last acquirer so a deadlock names its
 * holder instead of just its lock.
 */

#ifndef SPINLOCK_H
#define SPINLOCK_H

#include <stdint.h>
#include "ports.h"

typedef struct {
    volatile uint32_t locked;
    /* Last acquirer, stamped by the spin_lock() wrapper.  Deliberately
     * NOT cleared on unlock: under a deadlock the last acquirer is the
     * prime suspect, and skipping the store keeps unlock a single
     * release.  Diagnostic only -- racy reads are acceptable here. */
    volatile const char* owner_file;
    volatile uint32_t owner_line;
} spinlock_t;

#define SPINLOCK_INIT { 0 }

/* Set by task.c: invoked by the stuck-reporter below so scheduler
 * state can be dumped at deadlock time. */
extern void (*spin_stuck_hook)(const volatile void* lock_word);

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
                                         const volatile spinlock_t* l)
{
    const char* msg = "\n[[ ";
    for (const char* c = msg; *c; c++) write_serial(*c);
    for (const char* c = tag; *c; c++) write_serial(*c);
    const char* mid = " @ 0x";
    for (const char* c = mid; *c; c++) write_serial(*c);
    uintptr_t addr = (uintptr_t)&l->locked;
    for (int i = 28; i >= 0; i -= 4) {
        char nibble = (char)((addr >> i) & 0xF);
        write_serial((char)(nibble < 10 ? '0' + nibble : 'a' + nibble - 10));
    }
    /* Last acquirer: the whole point of the owner stamp. */
    const char* owner = (const char*)l->owner_file;
    if (owner) {
        const char* own = " holder=";
        for (const char* c = own; *c; c++) write_serial(*c);
        for (const char* c = owner; *c; c++) write_serial(*c);
        write_serial(':');
        uint32_t line = l->owner_line;
        char digits[10];
        int n = 0;
        do {
            digits[n++] = (char)('0' + line % 10);
            line /= 10;
        } while (line);
        while (n) write_serial(digits[--n]);
    }
    const char* tail = " ]]\n";
    for (const char* c = tail; *c; c++) write_serial(*c);
    /* Scheduler diagnostics live in task.c; a stuck scheduler lock is
     * exactly where you want the PCB dump. */
    void (*hook)(const volatile void*) = spin_stuck_hook;
    if (hook)
        hook(&l->locked);
}

static inline void spin_stuck_report(const volatile spinlock_t* l)
{
    spin_stuck_report_raw("SPINLOCK STUCK", l);
}
/* One-shot trace when a contender finds the lock taken: rare enough on
 * a healthy system to log without flooding, and it names the exact lock
 * word the moment contention begins. */
static inline void spin_contend_once(const volatile spinlock_t* l)
{
    static volatile uint32_t contend_shown_mask;
    uintptr_t slot = ((uintptr_t)&l->locked >> 4) & 31;
    uint32_t bit = 1u << slot;
    if (!(contend_shown_mask & bit)) {
        contend_shown_mask |= bit;
        spin_stuck_report_raw("SPINCONTEND", l);
    }
}
#else
#define SPIN_STUCK_LIMIT 0
static inline void spin_stuck_report(const volatile void* lock_word)
{
    (void)lock_word;
}
#endif

static inline void spin_init(spinlock_t* l)
{
    l->locked = 0;
    l->owner_file = 0;
    l->owner_line = 0;
}

/* Real acquire; spin_lock() stamps __FILE__/__LINE__ around it so the
 * stuck-reporter can name the last acquirer without touching any of
 * the call sites. */
static inline void spin_lock_tracked(spinlock_t* l,
                                     const char* file,
                                     uint32_t line)
{
    while (__sync_lock_test_and_set(&l->locked, 1)) {
        uint32_t spins = 0;
#ifdef JEXOS_DEBUG
        spin_contend_once(l);
#endif
        while (l->locked) {
            __asm__ volatile("pause");
            if (SPIN_STUCK_LIMIT && ++spins >= SPIN_STUCK_LIMIT) {
                spin_stuck_report(l);
                spins = 0;
            }
        }
    }
    l->owner_file = file;
    l->owner_line = line;
}

#define spin_lock(l) spin_lock_tracked((l), __FILE__, __LINE__)

static inline void spin_unlock(spinlock_t* l)
{
    __sync_lock_release(&l->locked);
}

/* irqsave variants for sections reachable with IF=1 */
static inline void spin_lock_irqsave_impl(spinlock_t* l,
                                          uint32_t* flags,
                                          const char* file,
                                          uint32_t line)
{
    *flags = irq_save();
    spin_lock_tracked(l, file, line);
}

#define spin_lock_irqsave(l, flags) \
    spin_lock_irqsave_impl((l), (flags), __FILE__, __LINE__)

static inline void spin_unlock_irqrestore(spinlock_t* l, uint32_t flags)
{
    spin_unlock(l);
    irq_restore(flags);
}

#endif // SPINLOCK_H
