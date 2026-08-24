/**
 * @file test_sync.c
 * @brief SMP synchronization self-tests.
 *
 * Purpose: Prove the concurrency primitives actually exclude and
 *          actually wake, on real cores.  These tests fork workers via
 *          kernel_fork_on() so both CPUs hammer shared state; any
 *          lost update, missed wakeup or broken handoff shows up as a
 *          count mismatch or a hung join.  A hang is diagnosable: the
 *          runner announces each test before it starts.
 */

#include "test_suite.h"
#include "kernel/atomic.h"
#include "kernel/mutex.h"
#include "kernel/completion.h"
#include "task.h"
#include <stdint.h>

/* ── Atomic hammer ──────────────────────────────────────
 * Both CPUs increment one counter without a lock; only the atomic
 * wrappers make the total exact. */

#define HAMMER_ITERATIONS 50000

static volatile uint32_t hammer_count;
static completion_t hammer_done[2];

static void atomic_hammer_worker(void* arg)
{
    uint32_t slot = (uint32_t)(uintptr_t)arg;
    for (int i = 0; i < HAMMER_ITERATIONS; i++)
        atomic_inc(&hammer_count);
    completion_signal(&hammer_done[slot]);
}

static void test_atomic_smp_hammer(void)
{
    hammer_count = 0;
    completion_init(&hammer_done[0]);
    completion_init(&hammer_done[1]);

    /* Pin one worker per CPU when possible; load-placed otherwise */
    int id0 = kernel_fork_on(atomic_hammer_worker, (void*)0, 0);
    TEST_ASSERT(id0 > 0, "fork cpu0 failed");
    int id1 = kernel_fork_on(atomic_hammer_worker, (void*)1, 1);
    TEST_ASSERT(id1 > 0, "fork cpu1 failed");

    completion_wait(&hammer_done[0]);
    completion_wait(&hammer_done[1]);

    uint32_t expect = 2u * HAMMER_ITERATIONS;
    TEST_ASSERT(hammer_count == expect, "lost updates in atomic_inc");
}

/* ── Mutex mutual exclusion ─────────────────────────────
 * Two workers bump a PLAIN counter inside the mutex.  If the lock
 * ever lets two holders in, the total comes up short. */

static mutex_t test_mutex;
static uint32_t racy_count;

static void mutex_exclusion_worker(void* arg)
{
    uint32_t slot = (uint32_t)(uintptr_t)arg;
    for (int i = 0; i < HAMMER_ITERATIONS / 2; i++) {
        mutex_lock(&test_mutex);
        racy_count++;               /* deliberately unprotected */
        mutex_unlock(&test_mutex);
    }
    completion_signal(&hammer_done[slot]);
}

static void test_mutex_exclusion(void)
{
    mutex_init(&test_mutex, "test-mutex");
    racy_count = 0;
    completion_init(&hammer_done[0]);
    completion_init(&hammer_done[1]);

    kernel_fork_on(mutex_exclusion_worker, (void*)0, 0);
    kernel_fork_on(mutex_exclusion_worker, (void*)1, 1);

    completion_wait(&hammer_done[0]);
    completion_wait(&hammer_done[1]);

    TEST_ASSERT(racy_count == HAMMER_ITERATIONS,
                "mutex allowed concurrent holders (lost updates)");
}

/* ── Completion semantics ───────────────────────────────
 * signal-before-wait must not be lost -- that is the whole reason
 * completions exist next to mutexes. */

static completion_t pre_signaled;

static void test_completion_early_signal(void)
{
    completion_init(&pre_signaled);
    completion_signal(&pre_signaled);   /* nobody waiting yet */
    completion_wait(&pre_signaled);     /* must consume and return */
}

void register_sync_tests(void)
{
    test_register("atomic_smp_hammer", "2 cpus x 50k atomic_inc = exact", test_atomic_smp_hammer);
    test_register("mutex_exclusion", "plain counter under mutex = exact", test_mutex_exclusion);
    test_register("completion_early_signal", "signal before wait survives", test_completion_early_signal);
}
