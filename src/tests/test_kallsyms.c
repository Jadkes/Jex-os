/**
 * @file test_kallsyms.c
 * @brief Self-tests for the packed kallsyms address resolver.
 *
 * Purpose: backtraces, the panic path and the new shell `sym` command
 * all lean on kallsyms_lookup; a bad binary-search bound or a broken
 * two-pass link silently degrades every crash report to "Unknown".
 * These tests resolve a real kernel symbol by its runtime address and
 * exercise exact-start, mid-function and miss paths.
 */

#include "test_suite.h"
#include <string.h>
#include <stdint.h>
#include "kernel/kallsyms.h"

extern void kallsyms_init(void);

static void test_kallsyms_lookup(void)
{
    uint32_t addr = (uint32_t)(uintptr_t)&kallsyms_init;
    uint32_t offset, size;

    const char* name = kallsyms_lookup(addr, &offset, &size);
    if (strcmp(name, "kallsyms_init") != 0 || offset != 0 || size == 0) {
        test_fail("exact symbol start not resolved");
        return;
    }

    name = kallsyms_lookup(addr + 1, &offset, &size);
    if (strcmp(name, "kallsyms_init") != 0 || offset != 1) {
        test_fail("mid-function address not contained");
        return;
    }
}

static void test_kallsyms_miss(void)
{
    uint32_t offset, size;
    const char* name = kallsyms_lookup(0x1000, &offset, &size);
    if (strcmp(name, "Unknown") != 0) {
        test_fail("unmapped low address resolved");
        return;
    }
}

void register_kallsyms_tests(void)
{
    test_register("kallsyms_hit", "resolve own init fn by address", test_kallsyms_lookup);
    test_register("kallsyms_miss", "unmapped address reports Unknown", test_kallsyms_miss);
}
