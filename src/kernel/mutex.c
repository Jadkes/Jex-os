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
#include "panic.h"
#include <stddef.h>

/* ── Mutex ────────────────────────────────────────────── */

void mutex_init(mutex_t* m, const char* name)
{
    m->held = 0;
    m->wait_head = NULL;
    m->wait_tail = NULL;
    m->name = name;
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
    if (in_isr || !interrupts_enabled()) {
        pr_err("mutex '%s': sleep attempted in %s\n",
               m->name ? m->name : "?",
               in_isr ? "ISR" : "irqs-off region");
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
            return;
        }

        /* Park: FIFO order keeps the lock fair-ish under load */
        task_t* self = cur_task();
        mutex_waiter_push(m, self);
        spin_unlock_irqrestore(&m->lock, flags);

        task_block_current();
        /* Woken -- loop and race for it again.  The re-check closes
         * every lost-wakeup window between park and switch. */
    }
}

void mutex_unlock(mutex_t* m)
{
    uint32_t flags;
    spin_lock_irqsave(&m->lock, &flags);

    if (!m->wait_head) {
        /* No contenders: plain release */
        m->held = 0;
        spin_unlock_irqrestore(&m->lock, flags);
        return;
    }

    /* Direct handoff: ownership moves to the first live waiter without
     * ever passing through unlocked. */
    task_t* w;
    while ((w = mutex_waiter_pop(m)) != NULL) {
        /* Corpses are dropped; a live waiter gets a direct handoff */
        if (w->state != STATE_ZOMBIE) {
            w->state = STATE_READY;
            spin_unlock_irqrestore(&m->lock, flags);
            return;
        }
    }

    /* Every waiter died while parked; nobody inherits */
    m->held = 0;
    spin_unlock_irqrestore(&m->lock, flags);
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

        spin_unlock_irqrestore(&c->lock, flags);
        task_block_current();
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
