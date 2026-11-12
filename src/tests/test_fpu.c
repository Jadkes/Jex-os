/**
 * @file test_fpu.c
 * @brief Per-task x87 state isolation.
 *
 * Purpose: prove that a task's floating-point registers and control word
 *          survive a switch to another task that uses the same unit.
 *          Without save/restore the last writer wins: the first worker
 *          would read the second worker's value and neither would notice
 *          anything but wrong numbers.
 *
 * Design: two kernel tasks pinned to CPU 0, so both use the same physical
 *          x87 unit and only the scheduler sits between them.  Each loads a
 *          distinct value into st(0) and a distinct control word, then
 *          sleeps across several ticks to force switches.  Afterwards each
 *          reads both back.  A third check runs in the test task itself: it
 *          never touched x87, so it must still see the pristine control
 *          word from its FPU image rather than whatever the workers left.
 *
 *          The opcodes are inline asm because kernel C is compiled with
 *          -mno-80387: the flag stops the compiler generating x87, not the
 *          programmer from testing it.
 */

#include "test_suite.h"
#include "kernel/completion.h"
#include "fpu.h"
#include "task.h"
#include "timer.h"
#include <stddef.h>
#include <stdint.h>

/* Enough ticks that both workers are guaranteed to preempt each other:
 * one 20 ms sleep already spans two LAPIC tick periods. */
#define FPU_SLEEP_ROUNDS    6
#define FPU_SLEEP_MS        20

/* 1.0f and 0.0f as raw bits; the values themselves never enter C. */
#define FPU_BITS_ZERO       0x00000000u
#define FPU_BITS_ONE        0x3F800000u

#ifndef FPU_CW_DEFAULT
#define FPU_CW_DEFAULT      0x037Fu   /* rounded, 64-bit precision, all masks */
#endif
#define FPU_CW_SINGLE       0x0E7Fu   /* round-to-nearest, single precision */

static completion_t fpu_done[2];
static uint32_t     fpu_value_seen[2];
static uint16_t     fpu_cw_seen[2];

static void fpu_load_bits(uint32_t bits)
{
    __asm__ volatile("flds %0" : : "m"(bits));
}

static uint32_t fpu_read_st0_bits(void)
{
    uint32_t bits;
    __asm__ volatile("fstps %0" : "=m"(bits));
    return bits;
}

static void fpu_write_cw(uint16_t cw)
{
    __asm__ volatile("fldcw %0" : : "m"(cw));
}

static uint16_t fpu_read_cw(void)
{
    uint16_t cw;
    __asm__ volatile("fnstcw %0" : "=m"(cw));
    return cw;
}

static void fpu_worker(void* arg)
{
    uint32_t slot = (uint32_t)(uintptr_t)arg;

    fpu_load_bits(slot ? FPU_BITS_ONE : FPU_BITS_ZERO);
    fpu_write_cw(slot ? FPU_CW_DEFAULT : FPU_CW_SINGLE);

    for (int i = 0; i < FPU_SLEEP_ROUNDS; i++)
        sleep(FPU_SLEEP_MS);

    fpu_value_seen[slot] = fpu_read_st0_bits();
    fpu_cw_seen[slot]    = fpu_read_cw();

    completion_signal(&fpu_done[slot]);
}

static void test_fpu_task_isolation(void)
{
    completion_init(&fpu_done[0]);
    completion_init(&fpu_done[1]);
    fpu_value_seen[0] = fpu_value_seen[1] = 0;
    fpu_cw_seen[0] = fpu_cw_seen[1] = 0;

    int id0 = kernel_fork_on(fpu_worker, (void*)(uintptr_t)0, 0);
    TEST_ASSERT(id0 > 0, "fork of FPU worker 0 failed");
    int id1 = kernel_fork_on(fpu_worker, (void*)(uintptr_t)1, 0);
    TEST_ASSERT(id1 > 0, "fork of FPU worker 1 failed");

    completion_wait(&fpu_done[0]);
    completion_wait(&fpu_done[1]);

    TEST_ASSERT(fpu_value_seen[0] == FPU_BITS_ZERO,
                "task 0 lost st(0) across a switch");
    TEST_ASSERT(fpu_value_seen[1] == FPU_BITS_ONE,
                "task 1 lost st(0) across a switch");
    TEST_ASSERT(fpu_cw_seen[0] == FPU_CW_SINGLE,
                "task 0 lost its x87 control word");
    TEST_ASSERT(fpu_cw_seen[1] == FPU_CW_DEFAULT,
                "task 1 lost its x87 control word");

    /* This task never used x87.  Its image came from the pristine template,
     * so a correct switch left it alone too. */
    TEST_ASSERT(fpu_read_cw() == FPU_CW_DEFAULT,
                "test task inherited another task's control word");
}

/* ── Control word hygiene on a fresh task ───────────────
 * A new task must start from the template, not from whatever the creating
 * task happened to leave in the unit.  The creating task here sets a
 * non-default control word, forks, and the child reports what it sees. */

static uint16_t fpu_child_cw_seen;
static uint16_t fpu_parent_cw_before;
static completion_t fpu_child_done;

static void fpu_child_worker(void* arg)
{
    (void)arg;
    fpu_child_cw_seen = fpu_read_cw();
    completion_signal(&fpu_child_done);
}

static void test_fpu_fresh_task_starts_pristine(void)
{
    completion_init(&fpu_child_done);

    uint16_t original = fpu_read_cw();
    fpu_write_cw(FPU_CW_SINGLE);
    fpu_parent_cw_before = fpu_read_cw();

    int id = kernel_fork_on(fpu_child_worker, NULL, 0);
    TEST_ASSERT(id > 0, "fork of FPU probe task failed");

    completion_wait(&fpu_child_done);

    /* The child must not inherit the single-precision word above. */
    TEST_ASSERT(fpu_parent_cw_before == FPU_CW_SINGLE,
                "parent could not set its control word");
    TEST_ASSERT(fpu_child_cw_seen == FPU_CW_DEFAULT,
                "fresh task did not start from the FPU template");

    fpu_write_cw(original);
}

static void register_fpu_tests_run(void)
{
    test_register("fpu_task_state", "x87 registers+control word survive switches",
                  test_fpu_task_isolation);
    test_register("fpu_fresh_task", "new task starts from the pristine FPU image",
                  test_fpu_fresh_task_starts_pristine);
}

void register_fpu_tests(void)
{
    if (!fpu_available()) {
        test_register_skip("fpu_task_state", "CPU reports no FPU", NULL);
        test_register_skip("fpu_fresh_task", "CPU reports no FPU", NULL);
        return;
    }
    register_fpu_tests_run();
}
