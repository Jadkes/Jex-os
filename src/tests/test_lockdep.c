/**
 * @file test_lockdep.c
 * @brief Self-tests for the lock dependency validator.
 *
 * Purpose: lockdep exists to catch inversions that would otherwise
 * deadlock the box; an undetected regression in the validator is a
 * silent return to "hang and pray" debugging.  These tests exercise
 * the pure ordering query, full acquire/release bookkeeping, real
 * cycle detection through the mutex hooks, and skewed-release
 * reporting.  Lock class names are unique per test because names are
 * the validator's identity key.
 */

#include "test_suite.h"
#include <string.h>
#include <stdint.h>
#include "kernel/lockdep.h"

/* Validator ships default-off (see header note); the unit tests turn
 * it on for their run and restore the previous state afterwards. */
static void with_lockdep(void (*fn)(void))
{
    int prev = lockdep_enabled;
    lockdep_enabled = 1;
    fn();
    lockdep_enabled = prev;
}

static void tld_order_query_impl(void)
{
    static struct lockdep_lock la = { .name = "tld_alpha" };
    static struct lockdep_lock lb = { .name = "tld_beta" };

    int before = lockdep_warnings;

    /* Fresh classes: taking beta while holding alpha records the edge */
    lockdep_acquire(&la, 0);
    if (lockdep_would_deadlock(&la, &lb) != 0) {
        test_fail("no-order pair reported as cycle");
        return;
    }
    lockdep_acquire(&lb, 0);            /* records tld_alpha -> tld_beta */
    lockdep_release(&lb);
    lockdep_release(&la);

    /* Now the reverse acquisition closes the recorded chain */
    lockdep_acquire(&lb, 0);
    int hit = lockdep_would_deadlock(&lb, &la);
    lockdep_release(&lb);

    if (hit != 1 || lockdep_warnings != before + 0) {
        test_fail("reverse order not detected");
        return;
    }
}

static void tld_abba_impl(void)
{
    static struct lockdep_lock lc = { .name = "tld_gamma" };
    static struct lockdep_lock ld = { .name = "tld_delta" };

    /* Teach gamma -> delta, then take delta while holding gamma:
     * acquire must warn instead of recording a poisoned edge. */
    lockdep_acquire(&lc, 0);
    lockdep_acquire(&ld, 0);
    lockdep_release(&ld);
    lockdep_release(&lc);

    int before = lockdep_warnings;
    lockdep_acquire(&ld, 0);
    lockdep_acquire(&lc, 0);            /* inversion: warn expected */
    lockdep_release(&lc);
    lockdep_release(&ld);

    if (lockdep_warnings != before + 1) {
        test_fail("abba inversion did not warn exactly once");
        return;
    }
}

static void tld_release_unheld_impl(void)
{
    static struct lockdep_lock le = { .name = "tld_eps" };

    int before = lockdep_warnings;
    lockdep_release(&le);               /* never acquired: warn */
    if (lockdep_warnings != before + 1) {
        test_fail("unheld release not reported");
        return;
    }
}

static void test_lockdep_order_query(void) { with_lockdep(tld_order_query_impl); }
static void test_lockdep_abba_warning(void) { with_lockdep(tld_abba_impl); }
static void test_lockdep_release_unheld(void) { with_lockdep(tld_release_unheld_impl); }

void register_lockdep_tests(void)
{
    test_register("lockdep_order", "query sees taught orderings", test_lockdep_order_query);
    test_register("lockdep_abba", "inverted acquire warns once", test_lockdep_abba_warning);
    test_register("lockdep_unheld", "skewed release reports", test_lockdep_release_unheld);
}
