/**
 * @file j3crash.c
 * @brief Crash simulator for the JexFS v3 journal.
 *
 * Purpose: prove that a transaction is all-or-nothing without booting
 * QEMU.  The tool logs a two-block metadata transaction (a superblock
 * generation bump in the primary and the backup), then copies the journal
 * back into a scratch image one byte-range at a time.  Each prefix is a
 * simulated crash point; replaying it must leave both superblocks on the
 * same generation, either the old one or the new one.
 *
 * The transaction is written by the same jexfs3_journal_write() the kernel
 * will use, and replayed by the same jexfs3_journal_replay() the kernel
 * mounts with, so this tests the real code rather than a model of it.
 *
 * Usage:
 *   j3crash <image.img> [--out DIR]
 *
 * With --out, every replayed image is written to DIR so jexfsck3 can
 * verify the whole filesystem, not just the generation field.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/stat.h>
#include "../src/include/jexfs3.h"
#include "../src/include/jexfs3_journal.h"

static uint8_t *base;
static uint8_t *logged;
static size_t image_size;
static uint32_t total_blocks;
static struct jexfs3_superblock sb;
static const char *out_dir;
static uint8_t *txn_sb;
static int failures;
static int cases;

static void die(const char *msg)
{
    fprintf(stderr, "Error: %s\n", msg);
    exit(1);
}

static int image_read(void *ctx, uint32_t block, void *buf)
{
    uint8_t *img = (uint8_t *)ctx;

    if (block >= total_blocks)
        return -1;
    memcpy(buf, img + (size_t)block * JEXFS3_BLOCK_SIZE, JEXFS3_BLOCK_SIZE);
    return 0;
}

static int image_write(void *ctx, uint32_t block, const void *buf)
{
    uint8_t *img = (uint8_t *)ctx;

    if (block >= total_blocks)
        return -1;
    memcpy(img + (size_t)block * JEXFS3_BLOCK_SIZE, buf, JEXFS3_BLOCK_SIZE);
    return 0;
}

static int load_image(const char *path)
{
    FILE *f = fopen(path, "rb");
    long size;

    if (!f)
        return -1;
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 0 || size % JEXFS3_BLOCK_SIZE != 0) {
        fclose(f);
        return -1;
    }
    image_size = (size_t)size;
    total_blocks = (uint32_t)(image_size / JEXFS3_BLOCK_SIZE);

    base = (uint8_t *)malloc(image_size);
    logged = (uint8_t *)malloc(image_size);
    if (!base || !logged) {
        fclose(f);
        return -1;
    }
    if (fread(base, 1, image_size, f) != image_size) {
        fclose(f);
        return -1;
    }
    fclose(f);
    memcpy(logged, base, image_size);
    return 0;
}

static int check_superblock(void)
{
    uint32_t crc;

    memcpy(&sb, base, sizeof(sb));
    if (sb.magic != JEXFS3_MAGIC || sb.version != JEXFS3_VERSION ||
        sb.block_size != JEXFS3_BLOCK_SIZE ||
        sb.total_blocks != total_blocks ||
        sb.total_bytes != (uint64_t)total_blocks * JEXFS3_BLOCK_SIZE)
        return -1;
    crc = jexfs3_crc_zeroed(base, JEXFS3_BLOCK_SIZE,
                            JEXFS3_SB_CHECKSUM_OFF);
    if (crc != sb.checksum)
        return -1;
    if (sb.journal_blocks < JEXFS3_JOURNAL_MIN_BLOCKS)
        return -1;
    if ((uint64_t)sb.journal_start + sb.journal_blocks >
        (uint64_t)sb.backup_block)
        return -1;
    if (sb.backup_block != total_blocks - 1)
        return -1;
    return 0;
}

/*
 * build_transaction - log a generation bump for the primary and backup
 * superblocks.
 *
 * A real metadata update would be an inode or bitmap block; the
 * superblock pair is used because both copies must stay byte-identical,
 * so applying only one of them is immediately visible as an
 * inconsistency.
 */
static uint32_t build_transaction(uint32_t *targets, const uint8_t **data)
{
    uint8_t *new_sb;
    uint32_t old_generation = sb.generation;

    new_sb = (uint8_t *)malloc(JEXFS3_BLOCK_SIZE);
    if (!new_sb)
        die("out of memory for the transaction");

    memcpy(new_sb, base, JEXFS3_BLOCK_SIZE);
    ((struct jexfs3_superblock *)new_sb)->generation = old_generation + 1u;
    ((struct jexfs3_superblock *)new_sb)->backup_generation =
        old_generation + 1u;
    ((struct jexfs3_superblock *)new_sb)->checksum =
        jexfs3_crc_zeroed(new_sb, JEXFS3_BLOCK_SIZE,
                          JEXFS3_SB_CHECKSUM_OFF);

    targets[0] = 0;
    targets[1] = sb.backup_block;
    data[0] = new_sb;
    data[1] = new_sb;
    txn_sb = new_sb;

    {
        struct jexfs3_journal_io io = {
            image_read, image_write, NULL, logged
        };
        if (jexfs3_journal_write(&sb, &io, targets, data, 2) != 0)
            die("journal write failed");
    }
    return old_generation + 1u;
}

static void write_image(const char *path, const uint8_t *img)
{
    FILE *f = fopen(path, "wb");

    if (!f)
        die("cannot write crash image");
    if (fwrite(img, 1, image_size, f) != image_size)
        die("short write on crash image");
    fclose(f);
}

static void check_case(uint32_t offset, uint32_t old_generation,
                       uint32_t new_generation, int index)
{
    uint8_t *crash = (uint8_t *)malloc(image_size);
    struct jexfs3_journal_io io = { image_read, image_write, NULL, crash };
    struct jexfs3_journal_info info;
    struct jexfs3_superblock *primary;
    struct jexfs3_superblock *backup;
    size_t journal_off = (size_t)(sb.journal_start + 1u) * JEXFS3_BLOCK_SIZE;
    size_t txn_bytes = (size_t)(2u + 2u) * JEXFS3_BLOCK_SIZE; /* desc + 2 data + commit */
    uint32_t gen_primary;
    uint32_t gen_backup;
    char path[1024];
    int rc;

    if (!crash)
        die("out of memory for a crash image");
    memcpy(crash, base, image_size);
    if (offset > txn_bytes)
        offset = (uint32_t)txn_bytes;
    memcpy(crash + journal_off, logged + journal_off, offset);

    rc = jexfs3_journal_replay(&sb, &io, &info);
    primary = (struct jexfs3_superblock *)crash;
    backup = (struct jexfs3_superblock *)(crash + (size_t)sb.backup_block *
                                          JEXFS3_BLOCK_SIZE);
    gen_primary = primary->generation;
    gen_backup = backup->generation;

    cases++;
    if (rc != 0) {
        fprintf(stderr, "  FAIL offset=%u: replay returned %d\n", offset, rc);
        failures++;
    } else if (gen_primary != gen_backup) {
        fprintf(stderr, "  FAIL offset=%u: torn superblock (primary %u, backup %u)\n",
                offset, gen_primary, gen_backup);
        failures++;
    } else if (gen_primary != old_generation &&
               gen_primary != new_generation) {
        fprintf(stderr, "  FAIL offset=%u: unexpected generation %u\n",
                offset, gen_primary);
        failures++;
    } else if (info.replayed && gen_primary != new_generation) {
        fprintf(stderr, "  FAIL offset=%u: replay claimed apply but kept old state\n",
                offset);
        failures++;
    } else if (!info.replayed && gen_primary != old_generation) {
        fprintf(stderr, "  FAIL offset=%u: state moved without a committed transaction\n",
                offset);
        failures++;
    }

    /* Every crash point must leave a superblock with a valid checksum. */
    if (jexfs3_crc_zeroed(crash, JEXFS3_BLOCK_SIZE,
                          JEXFS3_SB_CHECKSUM_OFF) != primary->checksum) {
        fprintf(stderr, "  FAIL offset=%u: primary checksum invalid after replay\n",
                offset);
        failures++;
    }

    if (out_dir) {
        snprintf(path, sizeof(path), "%s/crash-%03d-%05u.img",
                 out_dir, index, offset);
        write_image(path, crash);
    }
    printf("  offset %6u: %s\n", offset,
           gen_primary == new_generation ? "applied" : "rolled back");
    free(crash);
}

/*
 * check_partial_apply - simulate a crash after the commit block reached
 * the disk but in the middle of applying the targets.
 *
 * One superblock copy already carries the new generation and the other
 * still has the old one.  Replay must reapply the whole transaction from
 * the journal and clear the descriptor, leaving no torn pair behind.
 */
static void check_partial_apply(uint32_t new_generation, int index)
{
    uint8_t *crash = (uint8_t *)malloc(image_size);
    struct jexfs3_journal_io io = { image_read, image_write, NULL, crash };
    struct jexfs3_journal_info info;
    struct jexfs3_superblock *primary;
    struct jexfs3_superblock *backup;
    struct jexfs3_journal_desc desc;
    size_t journal_off = (size_t)(sb.journal_start + 1u) * JEXFS3_BLOCK_SIZE;
    char path[1024];
    int rc;

    if (!crash)
        die("out of memory for a partial-apply image");
    memcpy(crash, base, image_size);
    /* The whole transaction reached the journal... */
    memcpy(crash + journal_off, logged + journal_off,
           (size_t)4u * JEXFS3_BLOCK_SIZE);
    /* ...and the first target was applied before the crash. */
    memcpy(crash, txn_sb, JEXFS3_BLOCK_SIZE);

    primary = (struct jexfs3_superblock *)crash;
    backup = (struct jexfs3_superblock *)(crash + (size_t)sb.backup_block *
                                          JEXFS3_BLOCK_SIZE);

    rc = jexfs3_journal_replay(&sb, &io, &info);
    cases++;
    if (primary->generation != new_generation) {
        fprintf(stderr, "  FAIL partial apply: setup left generation %u\n",
                primary->generation);
        failures++;
    }
    if (rc != 0 || !info.replayed) {
        fprintf(stderr, "  FAIL partial apply: replay rc=%d replayed=%d\n",
                rc, info.replayed);
        failures++;
    }
    if (primary->generation != new_generation ||
        backup->generation != new_generation) {
        fprintf(stderr, "  FAIL partial apply: torn pair (primary %u, backup %u)\n",
                primary->generation, backup->generation);
        failures++;
    }
    memcpy(&desc, crash + journal_off, sizeof(desc));
    if (desc.magic == JEXFS3_JOURNAL_DESC_MAGIC) {
        fprintf(stderr, "  FAIL partial apply: descriptor not cleared\n");
        failures++;
    }

    if (out_dir) {
        snprintf(path, sizeof(path), "%s/crash-%03d-partial.img",
                 out_dir, index);
        write_image(path, crash);
    }
    printf("  partial apply: repaired\n");
    free(crash);
}

int main(int argc, char **argv)
{
    uint32_t targets[2];
    const uint8_t *data[2];
    uint32_t old_generation;
    uint32_t new_generation;
    int index = 0;

    if (argc < 2) {
        fprintf(stderr, "Usage: %s <image.img> [--out DIR]\n", argv[0]);
        return 1;
    }
    if (argc >= 4 && strcmp(argv[2], "--out") == 0)
        out_dir = argv[3];
    if (out_dir) {
        struct stat st;
        if (stat(out_dir, &st) != 0 || !S_ISDIR(st.st_mode))
            die("--out needs an existing directory");
    }
    if (load_image(argv[1]) != 0)
        die("cannot read image");
    if (check_superblock() != 0)
        die("not a v3 image or no journal");

    old_generation = sb.generation;
    new_generation = build_transaction(targets, data);

    printf("j3crash: journal %u blocks at %u, txn 2 blocks, generation %u -> %u\n",
           sb.journal_blocks, sb.journal_start, old_generation, new_generation);

    /* Every block boundary, plus the bytes where the descriptor, the
     * data and the commit turn meaningful. */
    check_case(0, old_generation, new_generation, index++);
    for (uint32_t b = 1; b <= 4u; b++) {
        uint32_t block_off = b * JEXFS3_BLOCK_SIZE;

        check_case(block_off, old_generation, new_generation, index++);
        if (b < 4u) {
            static const uint32_t within[] = { 1, 2, 4, 8, 12, 16, 64, 512, 2048 };
            for (unsigned i = 0; i < sizeof(within) / sizeof(within[0]); i++)
                check_case(block_off + within[i], old_generation,
                           new_generation, index++);
        }
    }
    check_partial_apply(new_generation, index++);

    printf("j3crash: %d case(s), %d failure(s)\n", cases, failures);
    free(txn_sb);
    free(base);
    free(logged);
    return failures ? 1 : 0;
}
