/**
 * @file test_string.c
 * @brief String, memory and printf-format self-tests.
 *
 * Purpose: The whole kernel leans on memcmp/memcpy/snprintf; a subtle
 *          regression there corrupts everything downstream.  These
 *          tests pin down the edge cases: empty ranges, truncation,
 *          and format specifiers used by the panic path.
 */

#include "test_suite.h"
#include <string.h>
#include "kernel/vsnprintf.h"
#include <stdint.h>

static void test_string_mem_ops(void)
{
    char a[16], b[16];
    memset(a, 0, sizeof(a));
    memcpy(b, "hello", 6);
    TEST_ASSERT(memcmp(a, b, 16) != 0, "memcmp saw equal buffers");
    TEST_ASSERT(strlen("hello") == 5, "strlen wrong");
    /* Zero-length compares must always be equal */
    TEST_ASSERT(memcmp(a, b, 0) == 0, "memcmp(0) not equal");
}

static void test_string_snprintf_truncate(void)
{
    char buf[8];
    int n = snprintf(buf, sizeof(buf), "%s", "truncate-me-please");
    /* Kernel contract: return value counts chars WRITTEN (not the
     * POSIX would-be length); output stays NUL-terminated */
    TEST_ASSERT(n == 7, "snprintf return != written length");
    TEST_ASSERT(strlen(buf) == 7, "snprintf did not truncate at size-1");
    TEST_ASSERT(buf[7] == '\0', "snprintf missing terminator");
}

static void test_string_formats(void)
{
    char buf[32];
    snprintf(buf, sizeof(buf), "%d", -42);
    TEST_ASSERT(strcmp(buf, "-42") == 0, "%%d negative broken");
    snprintf(buf, sizeof(buf), "%x", 0xDEAD);
    /* No width means fixed 8-digit hex in this kernel */
    TEST_ASSERT(strcmp(buf, "0000dead") == 0, "%%x broken");
    snprintf(buf, sizeof(buf), "%u", 4000000000u);
    TEST_ASSERT(strcmp(buf, "4000000000") == 0, "%%u large value broken");
    snprintf(buf, sizeof(buf), "%c%c", 'o', 'k');
    TEST_ASSERT(strcmp(buf, "ok") == 0, "%%c broken");
}

void register_string_tests(void)
{
    test_register("string_memops", "memset/memcpy/memcmp edges", test_string_mem_ops);
    test_register("snprintf_trunc", "truncation + terminator", test_string_snprintf_truncate);
    test_register("snprintf_fmts", "d/x/u/c formats", test_string_formats);
}
