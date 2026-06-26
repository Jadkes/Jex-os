/**
 * @file lockdep.c
 * @brief Lock Dependency Validator
 *
 * Detect potential deadlocks by tracking lock acquisition order.
 * Maintains an adjacency matrix of first-seen lock orderings and
 *         performs BFS cycle detection on every new acquisition. Warns on
 *         any inversion that would create a cycle.
 * Thread-safety: All public functions are safe under cli/sti save/restore.
 */

#define pr_fmt(fmt) "[LOCKDEP] " fmt
#include "kernel/printk.h"
#include "kernel/lockdep.h"
#include <stdint.h>
#include <stddef.h>

/*
 * Lock stack — tracks currently held locks in acquisition order.
 * depth allows the same lock to be acquired recursively (nesting).
 */
static struct {
    struct lockdep_lock* lock;
    unsigned int         depth;
} lock_stack[LOCKDEP_MAX_LOCKS];

static int lock_stack_depth = 0;

/*
 * Adjacency matrix: edges[i][j] == 1 means "lock at stack index i
 * was recorded as acquired before lock at stack index j".
 * Used for cycle detection.
 */
static uint8_t edges[LOCKDEP_MAX_LOCKS][LOCKDEP_MAX_LOCKS];

void lockdep_init(void)
{
    int i, j;
    for (i = 0; i < LOCKDEP_MAX_LOCKS; i++)
        for (j = 0; j < LOCKDEP_MAX_LOCKS; j++)
            edges[i][j] = 0;
    lock_stack_depth = 0;
}

/*
 * find_lock - Look up a lock in the current lock stack by pointer identity
 * @l: The lock to search for
 * @return Stack index on success, -1 if not held
 */
static int find_lock(struct lockdep_lock* l)
{
    int i;
    for (i = 0; i < lock_stack_depth; i++) {
        if (lock_stack[i].lock == l)
            return i;
    }
    return -1;
}

/**
 * check_deadlock - BFS for cycle detection
 * @from: Source stack index (the "before" lock)
 * @to:   Target stack index (the "after" lock)
 *
 * After recording that @from must be acquired before @to, search
 * for an existing path from @to back to @from. If one exists, the
 * new ordering creates a cycle and a potential deadlock.
 *
 * @return 1 if a cycle would be created, 0 otherwise
 */
static int check_deadlock(int from, int to)
{
    int visited[LOCKDEP_MAX_LOCKS];
    int queue[LOCKDEP_MAX_LOCKS];
    int qhead = 0, qtail = 0;
    int i;

    for (i = 0; i < LOCKDEP_MAX_LOCKS; i++)
        visited[i] = 0;

    queue[qtail++] = to;
    visited[to] = 1;

    while (qhead < qtail) {
        int cur = queue[qhead++];
        if (cur == from)
            return 1; /* cycle found */

        for (i = 0; i < LOCKDEP_MAX_LOCKS; i++) {
            if (edges[cur][i] && !visited[i]) {
                visited[i] = 1;
                queue[qtail++] = i;
            }
        }
    }
    return 0;
}

void lockdep_acquire(struct lockdep_lock* l, uint32_t ip)
{
    uint32_t eflags;
    __asm__ volatile("pushf; pop %0" : "=r"(eflags));
    __asm__ volatile("cli");

    l->ip = ip;

    /* Check if already held (recursive/nested acquire) */
    int idx = find_lock(l);
    if (idx >= 0) {
        lock_stack[idx].depth++;
        if (eflags & 0x200)
            __asm__ volatile("sti");
        return;
    }

    /*
     * Check ordering against all currently held locks.
     * For each held lock, record that it was acquired before the new one.
     */
    int new_idx = lock_stack_depth;
    int i;
    for (i = 0; i < new_idx; i++) {
        int from = i;
        int to   = new_idx;

        if (edges[from][to] == 0) {
            /* Record new ordering: lock[from] acquired before lock[to] */
            edges[from][to] = 1;

            /* Check if this creates a cycle */
            if (check_deadlock(from, to)) {
                pr_warn("POTENTIAL DEADLOCK: '%s' -> '%s' would create cycle!\n",
                        lock_stack[i].lock->name, l->name);
                pr_warn("  Caller IP: 0x%x\n", ip);
                /* Remove the edge to avoid persisting a bad record */
                edges[from][to] = 0;
            }
        }
    }

    /* Push onto lock stack — clear stale edges for reused index */
    if (new_idx < LOCKDEP_MAX_LOCKS) {
        for (i = 0; i < LOCKDEP_MAX_LOCKS; i++) {
            edges[new_idx][i] = 0;
            edges[i][new_idx] = 0;
        }
        lock_stack[new_idx].lock  = l;
        lock_stack[new_idx].depth = 1;
        lock_stack_depth++;
    }

    if (eflags & 0x200)
        __asm__ volatile("sti");
}

void lockdep_release(struct lockdep_lock* l)
{
    uint32_t eflags;
    __asm__ volatile("pushf; pop %0" : "=r"(eflags));
    __asm__ volatile("cli");

    int idx = find_lock(l);
    if (idx < 0) {
        if (eflags & 0x200)
            __asm__ volatile("sti");
        pr_warn("lockdep_release: '%s' not held (IP 0x%x)\n", l->name, l->ip);
        return;
    }

    if (lock_stack[idx].depth > 1) {
        /* Nested release — one level fewer */
        lock_stack[idx].depth--;
    } else {
        /* Remove this entry and shift everything down */
        int i;
        for (i = idx; i < lock_stack_depth - 1; i++) {
            lock_stack[i] = lock_stack[i + 1];
            /* Copy edges from the moved entry */
            int j;
            for (j = 0; j < LOCKDEP_MAX_LOCKS; j++) {
                edges[i][j] = edges[i + 1][j];
                edges[j][i] = edges[j][i + 1];
            }
        }
        /* Clear the now-unused last row/column */
        int last = lock_stack_depth - 1;
        for (i = 0; i < LOCKDEP_MAX_LOCKS; i++) {
            edges[last][i] = 0;
            edges[i][last] = 0;
        }
        lock_stack_depth--;
    }

    if (eflags & 0x200)
        __asm__ volatile("sti");
}

void lockdep_dump(void)
{
    int i;
    pr_info("Lock stack (%d deep):\n", lock_stack_depth);
    for (i = 0; i < lock_stack_depth; i++) {
        pr_info("  [%d] %s (depth=%u, ip=0x%x)\n",
                i, lock_stack[i].lock->name, lock_stack[i].depth,
                lock_stack[i].lock->ip);
    }
}

/*
 * Helper wrapper for simple spinlock-style locks.
 * Call lockdep_key_lock/unlock at the same places you would cli/sti.
 */

void lockdep_key_init(struct lockdep_key* key, const char* name)
{
    key->_dep.name = name;
    key->_dep.ip   = 0;
    key->held      = 0;
}

void lockdep_key_lock(struct lockdep_key* key)
{
    /*
     * Get caller IP from EBP chain.
     * EBP points to the saved frame pointer; return address is at EBP+4.
     * We dereference through EBP to get the caller's frame, then read
     * the return address from there.
     */
    uint32_t ip;
    __asm__ volatile("mov (%%ebp), %%eax; mov 4(%%eax), %0"
                     : "=r"(ip) :: "eax");
    lockdep_acquire(&key->_dep, ip);
    key->held = 1;
}

void lockdep_key_unlock(struct lockdep_key* key)
{
    lockdep_release(&key->_dep);
    key->held = 0;
}
