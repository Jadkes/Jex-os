/**
 * @file mutex.c
 * @brief Sleeping lock and event completion built on the scheduler.
 *
 * Both primitives share the same blocking pattern: enqueue the caller
 * on a FIFO under the object's spinlock, mark it BLOCKED, then yield
 * via task_switch().  Wakeup is cooperative -- the waker flips the
 * task back to READY and its CPU's next tick runs it.  Locks are never
 * held across the yield.
 */

#include "kernel/mutex.h"
#include "kernel/completion.h"
#include "task.h"
#include "ports.h"
#include "kernel/printk.h"
#include "kernel/lockdep.h"
#include "panic.h"
#include <stddef.h>

/* ── Mutex ────────────────────────────────────────────── */

#ifdef JEXOS_DEBUG
/* Path counters for the SMP hang hunts: a periodic timer dump of
 * these shows which transition stops happening when a mutex wedges. */
uint32_t dbg_mtx_fast, dbg_mtx_park, dbg_mtx_wake, dbg_mtx_plain;
#endif

void mutex_init(mutex_t* m, const char* name)
{
    m->held = 0;
    m->wait_head = NULL;
    m->wait_tail = NULL;
    m->name = name;
#ifdef JEXOS_DEBUG
    m->dep.name = name ? name : "?";
    m->dep.ip   = 0;
#endif
    m->lock = (spinlock_t)SPINLOCK_INIT;
}

static void mutex_waiter_push(mutex_t* m, task_t* t)
{
    t->wait_next = NULL;
    if (m->wait_tail)
        m->wait_tail->wait_next = t;
    else
        m->wait_head = t;
    m->wait_tail = t;
}

static task_t* mutex_waiter_pop(mutex_t* m)
{
    task_t* t = m->wait_head;
    if (!t)
        return NULL;
    m->wait_head = t->wait_next;
    if (!m->wait_head)
        m->wait_tail = NULL;
    t->wait_next = NULL;
    return t;
}

/* Sleeping with IF=0 or inside an ISR would park forever: no tick can
 * preempt us and the waker's task_wake flips a task that will never be
 * scheduled.  Turn that silent deadlock into a named panic instead. */
static void assert_can_sleep(const mutex_t* m)
{
    /*
     * Only THIS task's depth decides.  The global counter can be
     * stranded positive by a preempted victim frozen inside
     * irq_handler; consulting it here would make unrelated tasks
     * fail their sleep assert through no fault of their own.
     */
    task_t* self = cur_task();
    uint32_t irq_depth = self ? self->isr_depth : in_isr;
    if (irq_depth || !interrupts_enabled()) {
        pr_err("mutex '%s': sleep attempted in %s\n",
               m->name ? m->name : "?",
               (in_isr || irq_depth) ? "ISR" : "irqs-off region");
        panic_assert("atomic-context sleep", __FILE__, __LINE__);
    }
}

void mutex_lock(mutex_t* m)
{
    assert_can_sleep(m);
    for (;;) {
        uint32_t flags;
        spin_lock_irqsave(&m->lock, &flags);

        if (!m->held) {
            m->held = 1;
            spin_unlock_irqrestore(&m->lock, flags);
#ifdef JEXOS_DEBUG
            dbg_mtx_fast++;
            lockdep_acquire(&m->dep,
                            (uint32_t)__builtin_return_address(0));
#endif
            return;
        }

        /* Park: set BLOCKED and enqueue under the same lock so the
         * waker's READY write (also under m->lock) is serialized
         * against our BLOCKED write.  The old code released m->lock
         * first and then set BLOCKED under tasklist_lock -- a
         * different lock -- allowing the waker's READY to be
         * overwritten by our BLOCKED, causing a lost wakeup. */
        task_t* self = cur_task();
        self->state = STATE_BLOCKED;
        mutex_waiter_push(m, self);
        spin_unlock_irqrestore(&m->lock, flags);
#ifdef JEXOS_DEBUG
        dbg_mtx_park++;
#endif

        task_switch();
    }
}

void mutex_unlock(mutex_t* m)
{
#ifdef JEXOS_DEBUG
    lockdep_release(&m->dep);
#endif
    uint32_t flags;
    spin_lock_irqsave(&m->lock, &flags);

    /* Always release held before waking anyone.  The old "direct
     * handoff" left held=1 and just marked a waiter READY, but the
     * woken task re-contends instead of assuming ownership, so the
     * token was orphaned: nobody holds the mutex and nobody ever
     * unlocks it again.  Waking one waiter to race for a now-free
     * lock loses FIFO fairness but matches what mutex_lock's wake
     * path actually does. */
    m->held = 0;

    while (m->wait_head) {
        task_t* w = mutex_waiter_pop(m);
        if (w && w->state != STATE_ZOMBIE) {
            w->state = STATE_READY;
#ifdef JEXOS_DEBUG
            dbg_mtx_wake++;
#endif
            break;
        }
        /* Corpse parked on the wait list: drop it, try the next */
    }

    spin_unlock_irqrestore(&m->lock, flags);
#ifdef JEXOS_DEBUG
    dbg_mtx_plain++;
#endif
}

/* ── Completion ───────────────────────────────────────── */

void completion_init(completion_t* c)
{
    c->done = 0;
    c->wait_head = NULL;
    c->wait_tail = NULL;
    c->lock = (spinlock_t)SPINLOCK_INIT;
}

void completion_wait(completion_t* c)
{
    for (;;) {
        uint32_t flags;
        spin_lock_irqsave(&c->lock, &flags);

        if (c->done) {
            c->done = 0;   /* consume one signal */
            spin_unlock_irqrestore(&c->lock, flags);
            return;
        }

        task_t* self = cur_task();
        self->wait_next = NULL;
        if (c->wait_tail)
            c->wait_tail->wait_next = self;
        else
            c->wait_head = self;
        c->wait_tail = self;

        self->state = STATE_BLOCKED;
        spin_unlock_irqrestore(&c->lock, flags);
        task_switch();
    }
}

void completion_signal(completion_t* c)
{
    uint32_t flags;
    spin_lock_irqsave(&c->lock, &flags);
    c->done = 1;

    /* Wake everyone; whoever runs first consumes the flag */
    task_t* w = c->wait_head;
    while (w) {
        task_t* next = w->wait_next;
        if (w->state == STATE_BLOCKED)
            w->state = STATE_READY;
        w->wait_next = NULL;
        w = next;
    }
    c->wait_head = c->wait_tail = NULL;

    spin_unlock_irqrestore(&c->lock, flags);
}

void completion_reset(completion_t* c)
{
    uint32_t flags;
    spin_lock_irqsave(&c->lock, &flags);
    c->done = 0;
    spin_unlock_irqrestore(&c->lock, flags);
}
