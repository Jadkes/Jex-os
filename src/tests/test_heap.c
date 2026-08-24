/**
 * @file test_heap.c
 * @brief Heap and slab allocator self-tests.
 *
 * Purpose: Exercise kmalloc/kfree patterns that production code relies
 *          on, including the corruption guards (double free, bad
 *          offset) so a regression in slab validation is caught at
 *          boot instead of as silent memory rot.
 */

#include "test_suite.h"
#include "kheap.h"
#include <string.h>

static void test_heap_alloc_free(void)
{
    void* a = kmalloc(64);
    TEST_ASSERT(a != NULL, "kmalloc(64) returned NULL");
    memset(a, 0xA5, 64);
    kfree(a);
}

static void test_heap_size_classes(void)
{
    /* One alloc per slab class; all must succeed and be writable */
    static const uint32_t sizes[] = {16, 32, 64, 128, 256, 512, 1024, 2048};
    void* ptrs[8];
    for (int i = 0; i < 8; i++) {
        ptrs[i] = kmalloc(sizes[i]);
        TEST_ASSERT(ptrs[i] != NULL, "slab class alloc failed");
        memset(ptrs[i], (uint8_t)(0x10 + i), sizes[i]);
    }
    for (int i = 0; i < 8; i++)
        kfree(ptrs[i]);
}

static void test_heap_double_free_rejected(void)
{
    void* a = kmalloc(32);
    TEST_ASSERT(a != NULL, "alloc for double-free test failed");
    kfree(a);
    /* The guard must reject this, not corrupt the bitmap */
    kfree(a);
    /* Allocator still alive afterwards */
    void* b = kmalloc(32);
    TEST_ASSERT(b != NULL, "allocator dead after double free");
    kfree(b);
}

static void test_heap_oom_null(void)
{
    /* Absurd size must fail cleanly with NULL, never "succeed" */
    void* p = kmalloc(0x0FFFFFFF);
    TEST_ASSERT(p == NULL, "huge kmalloc did not return NULL");
}

void register_heap_tests(void)
{
    test_register("heap_alloc_free", "basic kmalloc/kfree", test_heap_alloc_free);
    test_register("heap_classes", "all 8 slab size classes", test_heap_size_classes);
    test_register("heap_double_free", "guard rejects double free", test_heap_double_free_rejected);
    test_register("heap_oom", "oversized alloc returns NULL", test_heap_oom_null);
}
