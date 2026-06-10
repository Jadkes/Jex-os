/**
 * @file test_suite.c
 * @brief Test runner — registers and runs kernel tests.
 */

#include "test_suite.h"
#include "terminal.h"
#include "serial.h"
#include <string.h>

extern void int_to_string(int n, char* str);

static test_t tests[TEST_MAX];
static int test_count = 0;
static int passed = 0;
static int failed = 0;
static int skipped = 0;

static char fail_msg[128];

void test_register(const char* name, const char* desc, test_fn_t fn)
{
    if (test_count >= TEST_MAX) return;
    tests[test_count].name = name;
    tests[test_count].description = desc;
    tests[test_count].fn = fn;
    tests[test_count].skipped = 0;
    test_count++;
}

void test_register_skip(const char* name, const char* desc, test_fn_t fn)
{
    if (test_count >= TEST_MAX) return;
    tests[test_count].name = name;
    tests[test_count].description = desc;
    tests[test_count].fn = fn;
    tests[test_count].skipped = 1;
    test_count++;
}

void test_pass(const char* name)
{
    passed++;
    terminal_writestring("  [PASS] ");
    terminal_writestring(name);
    terminal_writestring("\n");
}

void test_fail(const char* msg)
{
    failed++;
    if (msg) {
        strcpy(fail_msg, msg);
    }
    terminal_writestring("  [FAIL] ");
    terminal_writestring(msg);
    terminal_writestring("\n");
}

void test_skip(const char* name)
{
    skipped++;
    terminal_writestring("  [SKIP] ");
    terminal_writestring(name);
    terminal_writestring("\n");
}

void run_all_tests(void)
{
    char buf[16];
    passed = 0;
    failed = 0;
    skipped = 0;

    terminal_writestring("=== Running ");
    int_to_string(test_count, buf);
    terminal_writestring(buf);
    terminal_writestring(" tests ===\n");
    log_serial("=== Running tests ===\n");

    /* Register all test suites */
    register_checksum_tests();
    register_pmm_tests();

    for (int i = 0; i < test_count; i++) {
        if (tests[i].skipped) {
            test_skip(tests[i].name);
            continue;
        }
        int before_fail = failed;
        tests[i].fn();
        if (failed == before_fail) {
            test_pass(tests[i].name);
        }
    }

    terminal_writestring("\nResult: ");
    int_to_string(passed, buf);
    terminal_writestring(buf);
    terminal_writestring(" passed, ");
    int_to_string(failed, buf);
    terminal_writestring(buf);
    terminal_writestring(" failed, ");
    int_to_string(skipped, buf);
    terminal_writestring(buf);
    terminal_writestring(" skipped\n");

    log_serial("Tests complete\n");
}
