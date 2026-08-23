/*
 * atomic.h - Lightweight atomic integer operations
 *
 * Purpose: Single wrapper over the GCC __sync builtins so call sites
 *          read as intent (atomic_inc, atomic_or) instead of compiler
 *          intrinsics.  All operations are fully barriered (the __sync
 *          family implies a full fence), which is more than enough for
 *          this kernel's flag/count/bit-test-and-set uses.
 *
 * Thread-safety: Safe from any context -- ISR, AP, or task.  No locks
 *          are taken; these compile to locked instructions.
 */

#ifndef ATOMIC_H
#define ATOMIC_H

#include <stdint.h>

/**
 * atomic_inc - Increment an integer atomically
 * @v: pointer to the value
 */
static inline void atomic_inc(volatile int* v)
{
    __sync_fetch_and_add(v, 1);
}

/**
 * atomic_dec - Decrement an integer atomically
 * @v: pointer to the value
 */
static inline void atomic_dec(volatile int* v)
{
    __sync_fetch_and_sub(v, 1);
}

/**
 * atomic_inc_return - Increment and return the new value
 * @v: pointer to the value
 */
static inline int atomic_inc_return(volatile int* v)
{
    return __sync_add_and_fetch(v, 1);
}

/**
 * atomic_dec_return - Decrement and return the new value
 * @v: pointer to the value
 */
static inline int atomic_dec_return(volatile int* v)
{
    return __sync_sub_and_fetch(v, 1);
}

/**
 * atomic_add - Add n to an integer atomically
 * @v: pointer to the value
 * @n: amount to add
 */
static inline void atomic_add(volatile int* v, int n)
{
    __sync_fetch_and_add(v, n);
}

/**
 * atomic_xchg - Swap *v with n, returning the old value
 * @v: pointer to the value
 * @n: new value
 */
static inline int atomic_xchg(volatile int* v, int n)
{
    return __sync_lock_test_and_set(v, n);
}

/**
 * atomic_cmpxchg - If *v == expected, store desired; returns value seen
 * @v:       pointer to the value
 * @expected: value the caller believes is current
 * @desired:  replacement value
 *
 * Compare the return against @expected to know whether the swap
 * happened.
 */
static inline int atomic_cmpxchg(volatile int* v, int expected, int desired)
{
    return __sync_val_compare_and_swap(v, expected, desired);
}

/**
 * atomic_or - Set bits in a mask word atomically
 * @v:    pointer to the word
 * @mask: bits to set
 *
 * Used for signal_pending: the keyboard ISR and sys_kill race on the
 * same word, so plain |= loses updates.
 */
static inline void atomic_or(volatile uint32_t* v, uint32_t mask)
{
    __sync_fetch_and_or(v, mask);
}

/**
 * atomic_and - Clear bits in a mask word atomically
 * @v:    pointer to the word
 * @mask: bits to clear
 */
static inline void atomic_and(volatile uint32_t* v, uint32_t mask)
{
    __sync_fetch_and_and(v, mask);
}

#endif /* ATOMIC_H */
