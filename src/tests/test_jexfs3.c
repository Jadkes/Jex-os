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
#include "kernel/vsnprintf.h"
#include <jexos/errno.h>
#include <string.h>
#include <stdint.h>

#define V3_IMAGE_PATH "/jexfs3.img"
#define V3_DATA_BYTES 300000u
#define V3_INLINE_BYTES 200000u
#define V3_SPLIT_BYTES (16u * JEXFS3_BLOCK_SIZE)

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
    dev.read_block = image_read_block;
    dev.ctx = &source;
    dev.media_bytes = (uint64_t)bytes;


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

void register_jexfs3_tests(void)
{
    test_register("jexfs3_readonly",
                  "mount and verify a host-built v3 image",
                  test_jexfs3_readonly);
}
