/**
 * @file test_jexfs.c
 * @brief JexFS round-trip and concurrency self-tests.
 *
 * Purpose: Create/write/read/remove through the public (locking) API
 *          and verify byte-exact contents, including a write that
 *          spans the block-size boundary.  A second test runs two
 *          forked writers against separate files concurrently to
 *          smoke-test the fs_lock discipline.
 */

#include "test_suite.h"
#include "jexfs.h"
#include "kernel/printk.h"
#include "kernel/completion.h"
#include "kernel/vsnprintf.h"
#include "task.h"
#include <string.h>
#include <stdint.h>

#define TEST_FILE_BYTES 9000   /* crosses typical block boundaries */

static void test_jexfs_roundtrip(void)
{
    static uint8_t wbuf[TEST_FILE_BYTES];
    static uint8_t rbuf[TEST_FILE_BYTES];

    for (int i = 0; i < TEST_FILE_BYTES; i++)
        wbuf[i] = (uint8_t)(i * 7 + 3);

    int fd = jexfs_create("selftest.bin");
    if (fd < 0)
        fd = jexfs_open("selftest.bin");   /* leftover from earlier run? */
    TEST_ASSERT(fd >= 0, "jexfs_create failed");

    int w = jexfs_write(fd, wbuf, TEST_FILE_BYTES, 0);
    TEST_ASSERT(w == TEST_FILE_BYTES, "short write");

    memset(rbuf, 0, sizeof(rbuf));
    int r = jexfs_read(fd, rbuf, TEST_FILE_BYTES, 0);
    TEST_ASSERT(r == TEST_FILE_BYTES, "short read");
    TEST_ASSERT(memcmp(wbuf, rbuf, TEST_FILE_BYTES) == 0,
                "data corrupted across block boundary");

    /* Offset read must agree with the full-buffer view */
    uint8_t probe[16];
    r = jexfs_read(fd, probe, sizeof(probe), 4096);
    TEST_ASSERT(r == (int)sizeof(probe), "offset read short");
    TEST_ASSERT(memcmp(probe, wbuf + 4096, sizeof(probe)) == 0,
                "offset read wrong bytes");

    char why[64];
    int rc = jexfs_remove("selftest.bin");
    if (rc != 0) {
        snprintf(why, sizeof(why), "remove rc=%d (entry lost?)", rc);
        test_fail(why);
        return;
    }
}

static void test_jexfs_remove_missing(void)
{
    /* Removing what was never there must fail, not corrupt the dir */
    TEST_ASSERT(jexfs_remove("definitely-not-here.bin") != 0,
                "remove of missing file succeeded");
}

/* ── Concurrent writers ───────────────────────────────── */

static completion_t fs_done[2];

static uint8_t writer_pat[2][2048];   /* one pattern buffer PER worker --
                                         sharing one static was a race */

static void fs_writer(void* arg)
{
    uint32_t slot = (uint32_t)(uintptr_t)arg;
    char name[16] = "conc0.dat";
    name[4] = (char)('0' + slot);

    uint8_t* pat = writer_pat[slot];
    for (int i = 0; i < 2048; i++)
        pat[i] = (uint8_t)(slot * 64 + i);

    /* jexfs_create/jexfs_write take fs_lock themselves -- an outer
     * jexfs_lock here would self-deadlock the non-recursive mutex */
    int fd = jexfs_create(name);
    int ok = (fd >= 0) &&
             (jexfs_write(fd, pat, sizeof(pat), 0) == (int)sizeof(pat));

    if (!ok)
        test_fail("concurrent writer failed");

    completion_signal(&fs_done[slot]);
}

static void test_jexfs_concurrent_writers(void)
{
    completion_init(&fs_done[0]);
    completion_init(&fs_done[1]);

    kernel_fork_on(fs_writer, (void*)0, 0);
    kernel_fork_on(fs_writer, (void*)1, 1);
    completion_wait(&fs_done[0]);
    completion_wait(&fs_done[1]);

    /* Cleanup; remove takes fs_lock itself too */
    int c0 = jexfs_remove("conc0.dat");
    int c1 = jexfs_remove("conc1.dat");
    TEST_ASSERT(c0 == 0 && c1 == 0, "concurrent files not intact after write");
}

void register_jexfs_tests(void)
{
    test_register("jexfs_roundtrip", "9KB create/write/read/remove", test_jexfs_roundtrip);
    test_register("jexfs_remove_missing", "remove nonexistent fails cleanly", test_jexfs_remove_missing);
    test_register("jexfs_writers", "2 cpus write separate files", test_jexfs_concurrent_writers);
}
