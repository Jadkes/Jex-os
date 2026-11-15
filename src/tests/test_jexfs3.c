/**
 * @file test_jexfs3.c
 * @brief Read-only JexFS v3 mount self-test.
 *
 * The host build packs a v3 image into the test rootfs as /jexfs3.img.
 * Mount it through a block callback, run the driver's full consistency
 * check, then verify lookups, directory iteration and file contents
 * across an extent boundary.  In flavors without the image the test
 * reports itself skipped.
 */

#include "test_suite.h"
#include "jexfs.h"
#include "jexfs3_fs.h"
#include "jexfs3_journal.h"
#include "kernel/vsnprintf.h"
#include <jexos/errno.h>
#include <string.h>
#include <stdint.h>

#define V3_IMAGE_PATH "/jexfs3.img"
#define V3_DATA_BYTES 300000u
#define V3_INLINE_BYTES 200000u
#define V3_SPLIT_BYTES (16u * JEXFS3_BLOCK_SIZE)

/*
 * A separate, small image for the journal and write tests: 48 blocks
 * (192 KB) fits in a static buffer, which gives the tests a writable
 * device.  The host builds it with an 8-block journal, hello.txt and a
 * six-extent big.bin, so replacement can free both data and a leaf
 * extent block.
 */
#define V3_JOURNAL_IMAGE_PATH  "/jexfs3-journal.img"
#define V3_JOURNAL_IMAGE_BLOCKS 48u
#define V3_JOURNAL_IMAGE_BYTES (V3_JOURNAL_IMAGE_BLOCKS * JEXFS3_BLOCK_SIZE)

struct image_source {
    uint32_t inode;
    uint32_t bytes;
};

/*
 * image_read_block - serve a 4 KB v3 block from the v2 file.
 *
 * jexfs_read takes a 32-bit offset, which covers the 1 MB test image.
 */
static int image_read_block(void *ctx, uint32_t block, void *buf)
{
    struct image_source *src = (struct image_source *)ctx;
    uint64_t offset = (uint64_t)block * JEXFS3_BLOCK_SIZE;
    int got;

    if (offset + JEXFS3_BLOCK_SIZE > src->bytes)
        return -1;
    got = jexfs_read((int)src->inode, buf, JEXFS3_BLOCK_SIZE,
                     (uint32_t)offset);
    return got == (int)JEXFS3_BLOCK_SIZE ? 0 : -1;
}

static int buffer_is_byte(const uint8_t *buf, uint32_t len, uint8_t value)
{
    for (uint32_t i = 0; i < len; i++)
        if (buf[i] != value)
            return 0;
    return 1;
}

/*
 * RAM-backed device for the journal test.  The same callback shape as a
 * real disk lets the test exercise mount-time replay, including the
 * read-only refusal, without touching the rootfs image.
 */
struct memory_image {
    uint8_t *base;
    uint32_t blocks;
};

static int memory_read_block(void *ctx, uint32_t block, void *buf)
{
    struct memory_image *mem = (struct memory_image *)ctx;

    if (block >= mem->blocks)
        return -1;
    memcpy(buf, mem->base + (size_t)block * JEXFS3_BLOCK_SIZE,
           JEXFS3_BLOCK_SIZE);
    return 0;
}

static int memory_write_block(void *ctx, uint32_t block, const void *buf)
{
    struct memory_image *mem = (struct memory_image *)ctx;

    if (block >= mem->blocks)
        return -1;
    memcpy(mem->base + (size_t)block * JEXFS3_BLOCK_SIZE, buf,
           JEXFS3_BLOCK_SIZE);
    return 0;
}

static int memory_flush(void *ctx)
{
    (void)ctx;
    return 0;
}

/* Both journal tests mount this image, one after the other. */
static uint8_t journal_image[V3_JOURNAL_IMAGE_BYTES] __attribute__((aligned(16)));

static int load_journal_image(void)
{
    int fd = jexfs_open(V3_JOURNAL_IMAGE_PATH);

    if (fd < 0)
        return -1;
    if (jexfs_get_size(fd) != (int)V3_JOURNAL_IMAGE_BYTES)
        return -1;
    for (uint32_t block = 0; block < V3_JOURNAL_IMAGE_BLOCKS; block++) {
        int got = jexfs_read(fd, journal_image +
                             (size_t)block * JEXFS3_BLOCK_SIZE,
                             JEXFS3_BLOCK_SIZE,
                             block * JEXFS3_BLOCK_SIZE);
        if (got != (int)JEXFS3_BLOCK_SIZE)
            return -1;
    }
    return 0;
}

static void test_jexfs3_readonly(void)
{
    uint8_t buf[128];
    uint8_t pattern[64];
    struct image_source source;
    struct jexfs3_device dev;
    struct jexfs3_superblock sb;
    uint32_t inode;

    int fd = jexfs_open(V3_IMAGE_PATH);
    if (fd < 0) {
        test_skip("jexfs3_readonly (no image in this flavor)");
        return;
    }

    int bytes = jexfs_get_size(fd);
    TEST_ASSERT(bytes > 0, "v3 image is empty");

    source.inode = (uint32_t)fd;
    source.bytes = (uint32_t)bytes;
    dev = (struct jexfs3_device){
        .read_block = image_read_block,
        .ctx = &source,
        .media_bytes = (uint64_t)bytes,
    };


    int rc = jexfs3_mount(&dev);
    if (rc != 0) {
        char why[48];
        snprintf(why, sizeof(why), "v3 mount failed (%d)", rc);
        test_fail(why);
        return;
    }
    TEST_ASSERT(jexfs3_mounted(), "mount flag not set");
    rc = jexfs3_verify();
    if (rc != 0) {
        char why[48];
        snprintf(why, sizeof(why), "v3 consistency check failed (%d)", rc);
        test_fail(why);
        return;
    }
    TEST_ASSERT(jexfs3_superblock(&sb) == 0, "superblock read failed");
    TEST_ASSERT(sb.total_blocks == (uint32_t)(bytes / JEXFS3_BLOCK_SIZE),
                "superblock block count mismatch");
    TEST_ASSERT(sb.root_inode == JEXFS3_ROOT_INODE, "root inode mismatch");

    /* Small file: full read, short read and EOF behaviour. */
    TEST_ASSERT(jexfs3_lookup("/hello.txt", &inode) == 0,
                "hello.txt not found");
    memset(buf, 0, sizeof(buf));
    TEST_ASSERT(jexfs3_read_file(inode, buf, sizeof(buf), 0) == 13,
                "hello.txt short read");
    TEST_ASSERT(memcmp(buf, "hello jexfs3\n", 13) == 0,
                "hello.txt content wrong");
    TEST_ASSERT(jexfs3_read_file(inode, buf, 4, 0) == 4,
                "partial read from start failed");
    TEST_ASSERT(memcmp(buf, "hell", 4) == 0, "partial read wrong bytes");
    TEST_ASSERT(jexfs3_read_file(inode, buf, 8, 13) == 0,
                "read at EOF returned data");

    /* Nested path resolution. */
    TEST_ASSERT(jexfs3_lookup("/dir/sub/deep.txt", &inode) == 0,
                "nested file not found");
    memset(buf, 0, sizeof(buf));
    TEST_ASSERT(jexfs3_read_file(inode, buf, sizeof(buf), 0) == 12,
                "nested file short read");
    TEST_ASSERT(memcmp(buf, "deep jexfs3\n", 12) == 0,
                "nested file content wrong");

    /*
     * 300000-byte file stored as five 16-block extents: check the head,
     * a window straddling the first extent boundary, and the tail.
     */
    TEST_ASSERT(jexfs3_lookup("/data.bin", &inode) == 0,
                "data.bin not found");
    TEST_ASSERT(jexfs3_read_file(inode, pattern, sizeof(pattern), 0) ==
                (int)sizeof(pattern), "data.bin head read failed");
    TEST_ASSERT(buffer_is_byte(pattern, sizeof(pattern), 'A'),
                "data.bin head content wrong");

    memset(pattern, 0, sizeof(pattern));
    TEST_ASSERT(jexfs3_read_file(inode, pattern, sizeof(pattern),
                                 V3_SPLIT_BYTES - 32) ==
                (int)sizeof(pattern), "data.bin boundary read failed");
    TEST_ASSERT(buffer_is_byte(pattern, sizeof(pattern), 'A'),
                "data.bin boundary content wrong");

    memset(pattern, 0, sizeof(pattern));
    TEST_ASSERT(jexfs3_read_file(inode, pattern, sizeof(pattern),
                                 V3_DATA_BYTES - sizeof(pattern)) ==
                (int)sizeof(pattern), "data.bin tail read failed");
    TEST_ASSERT(buffer_is_byte(pattern, sizeof(pattern), 'A'),
                "data.bin tail content wrong");

    /*
     * A 200000-byte file split at 16 blocks becomes four inline extents
     * (16+16+16+1).  Exercising it proves multi-block inline reads cross
     * extent boundaries without a leaf block.
     */
    TEST_ASSERT(jexfs3_lookup("/inline.bin", &inode) == 0,
                "inline.bin not found");
    memset(pattern, 0, sizeof(pattern));
    TEST_ASSERT(jexfs3_read_file(inode, pattern, sizeof(pattern),
                                 V3_SPLIT_BYTES - 32) ==
                (int)sizeof(pattern), "inline extent boundary read failed");
    TEST_ASSERT(buffer_is_byte(pattern, sizeof(pattern), 'B'),
                "inline extent boundary content wrong");
    memset(pattern, 0, sizeof(pattern));
    TEST_ASSERT(jexfs3_read_file(inode, pattern, sizeof(pattern),
                                 V3_INLINE_BYTES - sizeof(pattern)) ==
                (int)sizeof(pattern), "inline extent tail read failed");
    TEST_ASSERT(buffer_is_byte(pattern, sizeof(pattern), 'B'),
                "inline extent tail content wrong");

    /* Directory iteration: exact order from the host builder. */
    TEST_ASSERT(jexfs3_lookup("/", &inode) == 0, "root lookup failed");
    {
        static const char *expected[] = {
            ".", "..", "data.bin", "dir", "hello.txt", "inline.bin"
        };
        uint64_t cookie = 0;
        int index = 0;

        for (;;) {
            char name[JEXFS3_NAME_MAX + 1];
            uint32_t child;
            uint8_t type;
            int rc = jexfs3_readdir(inode, &cookie, name, sizeof(name),
                                    &child, &type);

            if (rc == 0)
                break;
            TEST_ASSERT(rc == 1, "readdir failed");
            TEST_ASSERT(index < 6, "readdir returned extra entries");
            TEST_ASSERT(strcmp(name, expected[index]) == 0,
                        "readdir order/name mismatch");
            if (strcmp(name, ".") == 0) {
                TEST_ASSERT(child == inode, ". points elsewhere");
                TEST_ASSERT(type == JEXFS3_DIRENT_DIR, ". not a directory");
            }
            index++;
        }
        TEST_ASSERT(index == 6, "readdir missed entries");
    }

    /* Error paths. */
    TEST_ASSERT(jexfs3_lookup("/not-here", &inode) == -ENOENT,
                "missing path did not return ENOENT");
    TEST_ASSERT(jexfs3_lookup("/hello.txt/child", &inode) == -ENOTDIR,
                "descending through a file did not return ENOTDIR");

    jexfs3_unmount();
    TEST_ASSERT(!jexfs3_mounted(), "unmount left the filesystem mounted");
}

/*
 * log_generation_bump - log a two-block metadata transaction that raises
 * the generation in both superblock copies.
 *
 * The superblock pair is convenient because both copies must stay
 * byte-identical, so applying only one of them would fail the mount
 * check.  The transaction is written through the same journal code the
 * kernel will use for real metadata.
 */
static uint8_t txn_block[JEXFS3_BLOCK_SIZE] __attribute__((aligned(16)));

static int log_generation_bump(struct memory_image *mem,
                               const struct jexfs3_superblock *sb,
                               uint32_t next_generation)
{
    struct jexfs3_journal_io io = {
        memory_read_block, memory_write_block, memory_flush, mem
    };
    struct jexfs3_superblock *next =
        (struct jexfs3_superblock *)txn_block;
    uint32_t targets[2] = { 0u, sb->backup_block };
    const uint8_t *data[2] = { txn_block, txn_block };

    memcpy(txn_block, mem->base, JEXFS3_BLOCK_SIZE);
    next->generation = next_generation;
    next->backup_generation = next_generation;
    next->checksum = jexfs3_crc_zeroed(txn_block, JEXFS3_BLOCK_SIZE,
                                       JEXFS3_SB_CHECKSUM_OFF);
    return jexfs3_journal_write(sb, &io, targets, data, 2);
}

static void test_jexfs3_journal_replay(void)
{
    static uint8_t commit_copy[JEXFS3_BLOCK_SIZE] __attribute__((aligned(16)));
    struct memory_image mem;
    struct jexfs3_device dev;
    struct jexfs3_superblock sb;
    uint32_t generation;
    uint32_t commit_block;
    uint32_t inode;
    uint8_t buf[16];
    int bytes;

    if (load_journal_image() != 0) {
        test_skip("jexfs3_journal_replay (no image in this flavor)");
        return;
    }
    bytes = (int)V3_JOURNAL_IMAGE_BYTES;

    mem.base = journal_image;
    mem.blocks = V3_JOURNAL_IMAGE_BLOCKS;
    dev = (struct jexfs3_device){
        .read_block = memory_read_block,
        .write_block = memory_write_block,
        .flush = memory_flush,
        .ctx = &mem,
        .media_bytes = (uint64_t)bytes,
    };

    /* A clean journal must mount and read normally. */
    TEST_ASSERT(jexfs3_mount(&dev) == 0,
                "clean journal image failed to mount");
    TEST_ASSERT(jexfs3_superblock(&sb) == 0, "superblock read failed");
    generation = sb.generation;
    TEST_ASSERT(jexfs3_verify() == 0, "clean journal image failed verify");
    TEST_ASSERT(jexfs3_lookup("/hello.txt", &inode) == 0,
                "hello.txt missing from journal image");
    memset(buf, 0, sizeof(buf));
    TEST_ASSERT(jexfs3_read_file(inode, buf, sizeof(buf), 0) == 13,
                "journal image file short read");
    TEST_ASSERT(memcmp(buf, "hello jexfs3\n", 13) == 0,
                "journal image file content wrong");
    jexfs3_unmount();

    /* Log a generation bump that touches both superblock copies. */
    TEST_ASSERT(log_generation_bump(&mem, &sb, generation + 1u) == 0,
                "could not log a transaction");
    commit_block = sb.journal_start + 2u + 2u;   /* header, desc, 2 data */

    /* A transaction without its commit block must be ignored, not
     * half-applied. */
    memcpy(commit_copy, journal_image + (size_t)commit_block * JEXFS3_BLOCK_SIZE,
           JEXFS3_BLOCK_SIZE);
    memset(journal_image + (size_t)commit_block * JEXFS3_BLOCK_SIZE, 0,
           JEXFS3_BLOCK_SIZE);
    TEST_ASSERT(jexfs3_mount(&dev) == 0, "torn journal blocked the mount");
    TEST_ASSERT(jexfs3_superblock(&sb) == 0, "superblock read failed");
    TEST_ASSERT(sb.generation == generation,
                "torn transaction was applied");
    TEST_ASSERT(jexfs3_verify() == 0, "torn journal broke consistency");
    jexfs3_unmount();

    /* The same transaction with its commit block must be replayed. */
    memcpy(journal_image + (size_t)commit_block * JEXFS3_BLOCK_SIZE, commit_copy,
           JEXFS3_BLOCK_SIZE);
    TEST_ASSERT(jexfs3_mount(&dev) == 0, "committed journal failed to replay");
    TEST_ASSERT(jexfs3_superblock(&sb) == 0, "superblock read failed");
    TEST_ASSERT(sb.generation == generation + 1u,
                "replay did not apply the transaction");
    TEST_ASSERT(jexfs3_verify() == 0, "replayed image failed verify");
    TEST_ASSERT(jexfs3_lookup("/hello.txt", &inode) == 0,
                "replay lost the root directory");
    jexfs3_unmount();

    /* A second mount has nothing to replay and must not change state. */
    TEST_ASSERT(jexfs3_mount(&dev) == 0, "journal stayed dirty after replay");
    TEST_ASSERT(jexfs3_superblock(&sb) == 0, "superblock read failed");
    TEST_ASSERT(sb.generation == generation + 1u,
                "replay applied the transaction twice");
    jexfs3_unmount();

    /* A pending transaction on a read-only device is a mount failure:
     * serving stale metadata is worse than refusing to mount. */
    memcpy(&sb, journal_image, sizeof(sb));
    TEST_ASSERT(log_generation_bump(&mem, &sb, generation + 2u) == 0,
                "could not log the read-only transaction");
    dev.write_block = NULL;
    dev.flush = NULL;
    TEST_ASSERT(jexfs3_mount(&dev) == -EROFS,
                "dirty journal mounted on a read-only device");
    TEST_ASSERT(!jexfs3_mounted(), "failed mount left the filesystem mounted");
}

/*
 * Write-path test buffers.  The payload spans a partial final block and
 * the replacement of big.bin frees the old data plus its extent leaf.
 */
#define WRITE_TEST_BYTES  (3u * JEXFS3_BLOCK_SIZE + 123u)
static uint8_t write_payload[WRITE_TEST_BYTES];
static uint8_t write_readback[WRITE_TEST_BYTES];

static void fill_payload(uint8_t *buf, uint32_t len, uint8_t seed)
{
    for (uint32_t i = 0; i < len; i++)
        buf[i] = (uint8_t)(seed + (i * 7u));
}

static void test_jexfs3_write(void)
{
    struct memory_image mem;
    struct jexfs3_device dev;
    uint32_t inode;
    uint8_t head[16];

    if (load_journal_image() != 0) {
        test_skip("jexfs3_write (no image in this flavor)");
        return;
    }

    mem.base = journal_image;
    mem.blocks = V3_JOURNAL_IMAGE_BLOCKS;
    dev = (struct jexfs3_device){
        .read_block = memory_read_block,
        .write_block = memory_write_block,
        .flush = memory_flush,
        .ctx = &mem,
        .media_bytes = V3_JOURNAL_IMAGE_BYTES,
    };

    fill_payload(write_payload, sizeof(write_payload), 0x40);

    TEST_ASSERT(jexfs3_mount(&dev) == 0, "write test mount failed");
    TEST_ASSERT(jexfs3_lookup("/hello.txt", &inode) == 0,
                "hello.txt missing before write");

    /* Replace the small file with a three-block-plus-tail payload. */
    TEST_ASSERT(jexfs3_replace_file(inode, write_payload,
                                    sizeof(write_payload)) == 0,
                "replace hello.txt failed");
    memset(write_readback, 0, sizeof(write_readback));
    TEST_ASSERT(jexfs3_read_file(inode, write_readback,
                                 sizeof(write_readback), 0) ==
                (int)sizeof(write_payload),
                "read after replace was short");
    TEST_ASSERT(memcmp(write_readback, write_payload,
                       sizeof(write_payload)) == 0,
                "read after replace returned wrong bytes");
    TEST_ASSERT(jexfs3_verify() == 0, "verify failed after replace");
    jexfs3_unmount();

    /* The write must survive a remount without a journal replay. */
    TEST_ASSERT(jexfs3_mount(&dev) == 0, "remount after write failed");
    TEST_ASSERT(jexfs3_verify() == 0, "remount after write failed verify");
    TEST_ASSERT(jexfs3_lookup("/hello.txt", &inode) == 0,
                "hello.txt missing after remount");
    memset(write_readback, 0, sizeof(write_readback));
    TEST_ASSERT(jexfs3_read_file(inode, write_readback,
                                 sizeof(write_readback), 0) ==
                (int)sizeof(write_payload),
                "remounted file short read");
    TEST_ASSERT(memcmp(write_readback, write_payload,
                       sizeof(write_payload)) == 0,
                "remounted file content changed");
    jexfs3_unmount();

    /* big.bin used six one-block extents and a leaf extent block. */
    TEST_ASSERT(jexfs3_mount(&dev) == 0, "leaf-file mount failed");
    TEST_ASSERT(jexfs3_lookup("/big.bin", &inode) == 0,
                "big.bin missing before write");
    TEST_ASSERT(jexfs3_read_file(inode, head, sizeof(head), 0) ==
                (int)sizeof(head),
                "big.bin short read");
    TEST_ASSERT(buffer_is_byte(head, sizeof(head), 'C'),
                "big.bin initial content wrong");
    TEST_ASSERT(jexfs3_replace_file(inode, write_payload,
                                    sizeof(write_payload)) == 0,
                "replace big.bin failed");
    TEST_ASSERT(jexfs3_verify() == 0,
                "verify failed after replacing a leaf-extent file");
    jexfs3_unmount();

    TEST_ASSERT(jexfs3_mount(&dev) == 0, "remount after leaf replace failed");
    TEST_ASSERT(jexfs3_lookup("/big.bin", &inode) == 0,
                "big.bin missing after remount");
    memset(write_readback, 0, sizeof(write_readback));
    TEST_ASSERT(jexfs3_read_file(inode, write_readback,
                                 sizeof(write_readback), 0) ==
                (int)sizeof(write_payload),
                "leaf replacement short read");
    TEST_ASSERT(memcmp(write_readback, write_payload,
                       sizeof(write_payload)) == 0,
                "leaf replacement content wrong");
    TEST_ASSERT(jexfs3_verify() == 0,
                "leaf replacement failed verify after remount");

    /* Truncating to zero must release every block and read as EOF. */
    TEST_ASSERT(jexfs3_lookup("/hello.txt", &inode) == 0,
                "hello.txt missing before truncate");
    TEST_ASSERT(jexfs3_replace_file(inode, NULL, 0) == 0,
                "truncate failed");
    TEST_ASSERT(jexfs3_read_file(inode, write_readback, 1, 0) == 0,
                "truncated file still had data");
    TEST_ASSERT(jexfs3_verify() == 0, "verify failed after truncate");

    /* Directories are not replaceable. */
    TEST_ASSERT(jexfs3_lookup("/", &inode) == 0, "root lookup failed");
    TEST_ASSERT(jexfs3_replace_file(inode, write_payload, 1) == -EISDIR,
                "directory replacement was accepted");
    jexfs3_unmount();

    /* A read-only device must refuse writes instead of ignoring them. */
    dev.write_block = NULL;
    dev.flush = NULL;
    TEST_ASSERT(jexfs3_mount(&dev) == 0, "read-only mount failed");
    TEST_ASSERT(jexfs3_lookup("/big.bin", &inode) == 0,
                "big.bin missing before read-only check");
    TEST_ASSERT(jexfs3_replace_file(inode, write_payload, 1) == -EROFS,
                "read-only device accepted a write");
    jexfs3_unmount();
}

void register_jexfs3_tests(void)
{
    test_register("jexfs3_readonly",
                  "mount and verify a host-built v3 image",
                  test_jexfs3_readonly);
    test_register("jexfs3_journal_replay",
                  "replay a committed transaction and reject a torn one",
                  test_jexfs3_journal_replay);
    test_register("jexfs3_write",
                  "replace files, remount, truncate and free a leaf block",
                  test_jexfs3_write);
}
