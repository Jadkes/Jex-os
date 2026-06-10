/*
 * lockdep.h - Lock Dependency Validator
 *
 * Purpose: Detect potential deadlocks by tracking lock acquisition order.
 * Design: Maintains an adjacency matrix of first-seen lock orderings and
 *         performs BFS cycle detection on every new acquisition. Warns on
 *         any inversion that would create a cycle.
 * Thread-safety: All public functions are safe under cli/sti save/restore.
 */

#ifndef LOCKDEP_H
#define LOCKDEP_H

#include <stdint.h>

#define LOCKDEP_MAX_LOCKS 16

struct lockdep_lock {
    const char* name;    /* descriptive name for diagnostics */
    uint32_t    ip;      /* caller address (last acquire site) */
};

void lockdep_init(void);
void lockdep_acquire(struct lockdep_lock* l, uint32_t ip);
void lockdep_release(struct lockdep_lock* l);
void lockdep_dump(void);

/*
 * Helper wrapper for simple spinlock-style locks.
 * Embed a lockdep_lock for tracking and a held flag for sanity.
 */
struct lockdep_key {
    struct lockdep_lock _dep;  /* lockdep metadata embedded */
    int                held;
};

void lockdep_key_init(struct lockdep_key* key, const char* name);
void lockdep_key_lock(struct lockdep_key* key);
void lockdep_key_unlock(struct lockdep_key* key);

#endif /* LOCKDEP_H */
