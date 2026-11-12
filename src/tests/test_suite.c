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
    log_serial("[PASS] ");
    log_serial(name);
    log_serial("\n");
}

void test_fail(const char* msg)
{
    failed++;
    log_serial("[FAIL] ");
    log_serial(msg ? msg : "(no message)");
    log_serial("\n");
    if (msg) {
        strncpy(fail_msg, msg, sizeof(fail_msg) - 1);
        fail_msg[sizeof(fail_msg) - 1] = '\0';
    }
}

void test_skip(const char* name)
{
    skipped++;
    log_serial("[SKIP] ");
    log_serial(name);
    log_serial("\n");
}

static void announce(const char* tag, const char* name)
{
    terminal_writestring(tag);
    terminal_writestring(name);
    terminal_writestring("\n");
    /* Serial mirror: the test build is headless, so the log IS the UI */
    log_serial(tag);
    log_serial(name);
    log_serial("\n");
}

int run_selftests(void)
{
    char buf[16];
    passed = 0;
    failed = 0;
    skipped = 0;
    test_count = 0;

    register_checksum_tests();
    register_pmm_tests();
    register_heap_tests();
    register_ringbuf_tests();
    register_sync_tests();
    register_string_tests();
    register_kallsyms_tests();
    register_lockdep_tests();
    register_jexfs_tests();
    register_fpu_tests();

    int_to_string(test_count, buf);
    terminal_writestring("=== Running ");
    terminal_writestring(buf);
    terminal_writestring(" self-tests ===\n");
    log_serial("=== Running ");
    log_serial(buf);
    log_serial(" self-tests ===\n");

    for (int i = 0; i < test_count; i++) {
        if (tests[i].skipped) {
            test_skip(tests[i].name);
            continue;
        }
        /* Announce BEFORE running: a hang or fault leaves the guilty
         * test name as the last line in the log. */
        announce("[RUN  ] ", tests[i].name);
        int before_fail = failed;
        tests[i].fn();
        if (failed == before_fail) {
            test_pass(tests[i].name);
        }
    }

    int_to_string(passed, buf);
    terminal_writestring("\nResult: ");
    terminal_writestring(buf);
    int_to_string(failed, buf);
    terminal_writestring(" passed, ");
    terminal_writestring(buf);
    log_serial("selftest result: ");
    log_serial(buf);
    int_to_string(failed, buf);
    log_serial(" failed\n");
    return failed;
}

/* Compatibility with the interactive `runtests` shell command */
void run_all_tests(void)
{
    run_selftests();
}
