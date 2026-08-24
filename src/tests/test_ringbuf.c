/**
 * @file test_ringbuf.c
 * @brief SPSC ring buffer self-tests.
 *
 * Purpose: Hammer the wrap boundary and the empty/full edges of the
 *          byte ring that the keyboard driver depends on, so a mask or
 *          index regression shows up as a named failure.
 */

#include "test_suite.h"
#include "ringbuf.h"
#include <string.h>
#include <stdint.h>

#define RB_CAP 16   /* power of two */

static ringbuf_t rb;
static uint8_t storage[RB_CAP];

static void test_ringbuf_basic(void)
{
    ringbuf_init(&rb, storage, RB_CAP);
    TEST_ASSERT(!ringbuf_has_data(&rb), "fresh ring reports data");

    /* Contract: 0 on success, -1 on full; get gives -1 when empty */
    TEST_ASSERT(ringbuf_put(&rb, 0x42) == 0, "put into empty ring failed");
    TEST_ASSERT(ringbuf_has_data(&rb), "ring empty after put");
    int c = ringbuf_get(&rb);
    TEST_ASSERT(c == 0x42, "get returned wrong byte");
    TEST_ASSERT(!ringbuf_has_data(&rb), "ring not empty after drain");
}

static void test_ringbuf_wrap(void)
{
    ringbuf_init(&rb, storage, RB_CAP);

    /* Push a full cap + 3*wrap worth through; every byte must survive
     * the index wraparound untouched */
    uint8_t expect = 0;
    for (uint32_t i = 0; i < RB_CAP * 4 + 3; i++) {
        TEST_ASSERT(ringbuf_put(&rb, expect) == 0, "put failed mid-stream");
        int got = ringbuf_get(&rb);
        TEST_ASSERT(got == expect, "byte corrupted across wrap");
        expect = (uint8_t)(expect + 1);
    }
}

static void test_ringbuf_full_drops(void)
{
    ringbuf_init(&rb, storage, RB_CAP);

    /* One slot is kept empty to distinguish full from empty, so the
     * usable capacity is RB_CAP - 1 */
    for (int i = 0; i < RB_CAP - 1; i++)
        TEST_ASSERT(ringbuf_put(&rb, (uint8_t)i) == 0, "fill put failed");
    /* Full: further puts must fail without touching the tail */
    TEST_ASSERT(ringbuf_put(&rb, 0xFF) != 0, "put into full ring succeeded");

    /* FIFO order preserved */
    for (int i = 0; i < RB_CAP - 1; i++) {
        int c = ringbuf_get(&rb);
        TEST_ASSERT(c == i, "FIFO order broken when full-drop happened");
    }
    TEST_ASSERT(ringbuf_get(&rb) == -1, "drained ring still yields bytes");
}

void register_ringbuf_tests(void)
{
    test_register("ringbuf_basic", "empty/one-byte semantics", test_ringbuf_basic);
    test_register("ringbuf_wrap", "67 bytes across 3 wraps", test_ringbuf_wrap);
    test_register("ringbuf_full", "full drops new, keeps FIFO", test_ringbuf_full_drops);
}
