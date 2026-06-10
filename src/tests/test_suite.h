/**
 * @file test_suite.h
 * @brief Kernel test registration and runner.
 *
 * Purpose: Provide a lightweight test framework for in-kernel testing
 *          without userspace or QEMU exit. Tests run in kernel context.
 */

#ifndef TEST_SUITE_H
#define TEST_SUITE_H

#include <stdint.h>

typedef void (*test_fn_t)(void);

typedef struct {
    const char* name;
    const char* description;
    test_fn_t   fn;
    int         skipped;
} test_t;

#define TEST_MAX 32

void test_register(const char* name, const char* desc, test_fn_t fn);
void test_register_skip(const char* name, const char* desc, test_fn_t fn);
void run_all_tests(void);

/* Test helper macros */
#define TEST_ASSERT(cond, msg) \
    do { \
        if (!(cond)) { \
            test_fail(msg); \
            return; \
        } \
    } while(0)

void test_pass(const char* name);
void test_fail(const char* msg);
void test_skip(const char* name);

/* Declare all test registration functions */
void register_checksum_tests(void);
void register_pmm_tests(void);

#endif /* TEST_SUITE_H */
