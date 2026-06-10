/**
 * @file test_pmm.c
 * @brief Test suite for Physical Memory Manager.
 */

#include "test_suite.h"
#include "pmm.h"
#include <string.h>

static void test_pmm_alloc_free(void)
{
    void* block = pmm_alloc_block();
    TEST_ASSERT(block != NULL, "pmm_alloc_block should return non-NULL");

    /* Write to the block to verify it's mapped */
    memset(block, 0xAA, 4096);

    /* Verify all bytes were written */
    uint8_t* b = (uint8_t*)block;
    int ok = 1;
    for (int i = 0; i < 4096; i++) {
        if (b[i] != 0xAA) { ok = 0; break; }
    }
    TEST_ASSERT(ok, "allocated block should be writable");
}

void register_pmm_tests(void)
{
    test_register("pmm_alloc", "PMM allocate and validate block", test_pmm_alloc_free);
}
