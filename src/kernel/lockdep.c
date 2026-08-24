/**
 * @file lockdep.c
 * @brief Lock Dependency Validator — detects lock-order inversions.
 *
 * Every mutex acquisition is validated against a persistent table of
 * observed orderings (edges keyed by lock name = lock class).  Taking
 * lock B while holding A records "A before B"; if a chain already
 * requires B before A, the acquisition closes a cycle and we warn at
 * acquire time — before anyone gets to park forever.
 *
 * History: this file was rewritten.  The previous implementation kept
 * ONE global lock stack shared by every task and cpu and indexed its
 * adjacency matrix by stack POSITION, so concurrent holders corrupted
 * each other's state; it also had no callers.  The rewrite gives each
 * task its own held-stack and keys edges by stable class names.
 *
 * Thread-safety: one irqsave spinlock covers all validator state;
 * callable from any task on any cpu.  Not for ISR context — locks
 * validated here sleep, so their callers are task context anyway.
 */

#define pr_fmt(fmt) "[LOCKDEP] " fmt
#include "kernel/printk.h"
#include "kernel/lockdep.h"
#include "spinlock.h"
#include "task.h"
#include <stdint.h>
#include <stddef.h>

int lockdep_warnings = 0;
int lockdep_enabled = 0;



struct held_stack {
    void* owner;                                  /* NULL = free slot */
    struct lockdep_lock* held[LOCKDEP_MAX_HELD];  /* acquisition order */
    int depth;
};

static struct held_stack task_stacks[LOCKDEP_MAX_TASKS];

struct dep_edge {
    const char* from;   /* must always be taken BEFORE .to */
    const char* to;
};

static struct dep_edge edges[LOCKDEP_MAX_EDGES];
static int edge_count;

static spinlock_t dep_lock = SPINLOCK_INIT;

void lockdep_init(void)
{
    for (int i = 0; i < LOCKDEP_MAX_TASKS; i++) {
        task_stacks[i].owner = NULL;
        task_stacks[i].depth = 0;
    }
    edge_count = 0;
    lockdep_warnings = 0;
}

/*
 * slot_for - the calling task's held stack, claiming a free slot on
 * first use.  Returns NULL when the pool is exhausted; such tasks run
 * untracked rather than breaking the kernel.
 */
static struct held_stack* slot_for(task_t* self)
{
    for (int i = 0; i < LOCKDEP_MAX_TASKS; i++)
        if (task_stacks[i].owner == (void*)self)
            return &task_stacks[i];

    for (int i = 0; i < LOCKDEP_MAX_TASKS; i++) {
        if (task_stacks[i].owner == NULL) {
            task_stacks[i].owner = (void*)self;
            task_stacks[i].depth = 0;
            return &task_stacks[i];
        }
    }
    return NULL;
}

/*
 * path_exists - is there a recorded ordering chain requiring @a to be
 * taken before @b?  Names are compared by pointer identity: every
 * lock of one class shares the same static name string.
 */
static int path_exists(const char* a, const char* b)
{
    if (a == b)
        return 1;

    /* Room for one enqueue per edge in both the seed and expand
     * phases; the bound makes overflow impossible. */
    const char* queue[LOCKDEP_MAX_EDGES * 2];
    int qhead = 0, qtail = 0;

    for (int i = 0; i < edge_count; i++)
        if (edges[i].from == a)
            queue[qtail++] = edges[i].to;

    while (qhead < qtail) {
        const char* cur = queue[qhead++];
        if (cur == b)
            return 1;
        for (int i = 0; i < edge_count; i++) {
            if (edges[i].from != cur)
                continue;
            int pending = 0;
            for (int j = qhead; j < qtail; j++) {
                if (queue[j] == edges[i].to) {
                    pending = 1;
                    break;
                }
            }
            if (!pending && qtail < (int)(sizeof(queue) / sizeof(queue[0])))
                queue[qtail++] = edges[i].to;
        }
    }
    return 0;
}

static int edge_known(const char* from, const char* to)
{
    for (int i = 0; i < edge_count; i++)
        if (edges[i].from == from && edges[i].to == to)
            return 1;
    return 0;
}

static void record_edge(const char* from, const char* to)
{
    if (edge_count >= LOCKDEP_MAX_EDGES) {
        static int full_warned;
        if (!full_warned) {
            full_warned = 1;
            pr_warn("ordering table full (%d edges); further "
                    "orderings go unvalidated\n", LOCKDEP_MAX_EDGES);
        }
        return;
    }
    edges[edge_count].from = from;
    edges[edge_count].to   = to;
    edge_count++;
}

int lockdep_would_deadlock(struct lockdep_lock* held,
                           struct lockdep_lock* want)
{
    const char* held_name = held->name ? held->name : "?";
    const char* want_name = want->name ? want->name : "?";

    if (!lockdep_enabled)
        return path_exists(want_name, held_name);

    uint32_t flags;
    int cycles;

    spin_lock_irqsave(&dep_lock, &flags);
    cycles = path_exists(want_name, held_name);
    spin_unlock_irqrestore(&dep_lock, flags);
    return cycles;
}

void lockdep_acquire(struct lockdep_lock* l, uint32_t ip)
{
    const char* name = l->name ? l->name : "?";
    uint32_t flags;

    if (!lockdep_enabled)
        return;

    spin_lock_irqsave(&dep_lock, &flags);

    task_t* self = cur_task();
    struct held_stack* st = self ? slot_for(self) : NULL;
    if (!st) {
        spin_unlock_irqrestore(&dep_lock, flags);
        return;
    }
    for (int i = 0; i < st->depth; i++) {
        if (st->held[i] == l) {
            /* Mutexes here are non-recursive: re-acquiring while
             * held is the classic self-deadlock pattern. */
            pr_warn("'%s' acquired while already held\n", name);
            lockdep_warnings++;
            spin_unlock_irqrestore(&dep_lock, flags);
            return;
        }
    }

    if (st->depth < LOCKDEP_MAX_HELD) {
        for (int i = 0; i < st->depth; i++) {
            const char* from = st->held[i]->name ? st->held[i]->name : "?";
            if (edge_known(from, name))
                continue;
            if (path_exists(name, from)) {
                pr_warn("POTENTIAL DEADLOCK: taking '%s' while holding "
                        "'%s' reverses an existing order\n", name, from);
                lockdep_warnings++;
                continue;
            }
            record_edge(from, name);
        }
        l->ip = ip;
        st->held[st->depth++] = l;
    }

    spin_unlock_irqrestore(&dep_lock, flags);
}

void lockdep_release(struct lockdep_lock* l)
{
    const char* name = l->name ? l->name : "?";
    uint32_t flags;

    if (!lockdep_enabled)
        return;

    spin_lock_irqsave(&dep_lock, &flags);

    task_t* self = cur_task();
    struct held_stack* st = self ? slot_for(self) : NULL;
    int idx = -1;
    if (st) {
        for (int i = 0; i < st->depth; i++) {
            if (st->held[i] == l) {
                idx = i;
                break;
            }
        }
    }

    if (idx < 0) {
        pr_warn("release of unheld '%s'\n", name);
        lockdep_warnings++;
    } else {
        /* Preserve relative order of the remaining holds: they still
         * pair into edge checks on the next acquisition. */
        for (int i = idx; i < st->depth - 1; i++)
            st->held[i] = st->held[i + 1];
        st->depth--;
    }

    spin_unlock_irqrestore(&dep_lock, flags);
}

void lockdep_task_exit(void* task)
{
    uint32_t flags;
    spin_lock_irqsave(&dep_lock, &flags);
    for (int i = 0; i < LOCKDEP_MAX_TASKS; i++) {
        if (task_stacks[i].owner == task) {
            if (task_stacks[i].depth > 0)
                pr_warn("exiting task still holds %d lock(s)\n",
                        task_stacks[i].depth);
            task_stacks[i].owner = NULL;
            task_stacks[i].depth = 0;
        }
    }
    spin_unlock_irqrestore(&dep_lock, flags);
}

void lockdep_dump(void)
{
    task_t* self = cur_task();
    struct held_stack* st = self ? slot_for(self) : NULL;

    pr_info("current task holds %d lock(s)\n", st ? st->depth : 0);
    if (st) {
        for (int i = 0; i < st->depth; i++)
            pr_info("  [%d] %s (ip 0x%x)\n", i,
                    st->held[i]->name ? st->held[i]->name : "?",
                    st->held[i]->ip);
    }
    pr_info("%d recorded ordering edge(s):\n", edge_count);
    for (int i = 0; i < edge_count; i++)
        pr_info("  '%s' before '%s'\n", edges[i].from, edges[i].to);
}
