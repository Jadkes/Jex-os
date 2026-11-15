/**
 * @file j3crash.c
 * @brief Crash simulator for the JexFS v3 journal and file writer.
 *
 * Purpose: prove that a transaction is all-or-nothing without booting
 * QEMU.  Two scenarios run:
 *
 *   1. A two-block superblock generation bump is logged, then copied back
 *      into a scratch image one byte-range at a time.  Replay must leave
 *      the pair on the old generation or the new one.
 *   2. jexfs3_image_replace_file() rewrites a real file through a write
 *      callback that fails after N writes.  Reboot replay must leave the
 *      old file or the complete new one.
 *
 * Both scenarios use the same journal and writer code the kernel uses,
 * so they test the real rules rather than a model of them.
 *
 * Usage:
 *   j3crash <image.img> [--out DIR]
 *
 * With --out, every replayed image is written to DIR so jexfsck3 can
 * verify the whole filesystem, not just one inode.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/stat.h>
#include "../src/include/jexfs3.h"
#include "../src/include/jexfs3_journal.h"
#include "../src/include/jexfs3_write.h"

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

static int inode_at(const uint8_t *img, uint32_t index,
                    struct jexfs3_inode *out)
{
    uint64_t byte_off;
    uint32_t block;
    uint32_t within;

    if (index >= sb.inode_count)
        return -1;
    byte_off = (uint64_t)index * JEXFS3_INODE_SIZE;
    block = sb.inode_table_start + (uint32_t)(byte_off >> 12);
    within = (uint32_t)(byte_off & (JEXFS3_BLOCK_SIZE - 1u));
    if ((uint64_t)block * JEXFS3_BLOCK_SIZE + within + JEXFS3_INODE_SIZE >
        image_size)
        return -1;
    memcpy(out, img + (size_t)block * JEXFS3_BLOCK_SIZE + within,
           sizeof(*out));
    if (jexfs3_crc_zeroed(out, JEXFS3_INODE_SIZE,
                          JEXFS3_INODE_CHECKSUM_OFF) != out->checksum)
        return -1;
    return 0;
}

static int inode_block_for(const uint8_t *img,
                           const struct jexfs3_inode *inode,
                           uint32_t logical, uint32_t *physical)
{
    uint8_t leaf[JEXFS3_BLOCK_SIZE];
    uint32_t i;

    if (inode->extent_depth == 0) {
        for (i = 0; i < inode->extent_count; i++) {
            const struct jexfs3_extent *ext = &inode->inline_extents[i];

            if (logical >= ext->logical &&
                (uint64_t)logical < (uint64_t)ext->logical + ext->length) {
                *physical = ext->physical + (logical - ext->logical);
                return 0;
            }
        }
        return -1;
    }
    if (inode->extent_depth != 1 ||
        inode->extent_root >= total_blocks)
        return -1;
    memcpy(leaf, img + (size_t)inode->extent_root * JEXFS3_BLOCK_SIZE,
           JEXFS3_BLOCK_SIZE);
    {
        const struct jexfs3_extent_block *eb =
            (const struct jexfs3_extent_block *)leaf;

        if (eb->magic != JEXFS3_EXTENT_MAGIC || eb->depth != 1 ||
            eb->count > JEXFS3_EXTENT_ENTRIES)
            return -1;
        for (i = 0; i < eb->count; i++) {
            const struct jexfs3_extent *ext = &eb->entries[i];

            if (logical >= ext->logical &&
                (uint64_t)logical < (uint64_t)ext->logical + ext->length) {
                *physical = ext->physical + (logical - ext->logical);
                return 0;
            }
        }
    }
    return -1;
}

/*
 * find_file_inode - resolve a name in the root directory.
 *
 * The test image's root directory fits one block; this is a test helper,
 * not a general path walker.
 */
static uint32_t find_file_inode(const uint8_t *img, const char *name)
{
    struct jexfs3_inode root;
    uint8_t block[JEXFS3_BLOCK_SIZE];
    uint64_t pos = 0;
    uint64_t limit;
    uint32_t physical;

    if (inode_at(img, sb.root_inode, &root) != 0)
        return 0;
    if ((root.mode & JEXFS3_TYPE_MASK) != JEXFS3_TYPE_DIR)
        return 0;
    if (inode_block_for(img, &root, 0, &physical) != 0)
        return 0;
    memcpy(block, img + (size_t)physical * JEXFS3_BLOCK_SIZE,
           JEXFS3_BLOCK_SIZE);

    limit = root.size < JEXFS3_BLOCK_SIZE ? root.size : JEXFS3_BLOCK_SIZE;
    while (pos + JEXFS3_DIRENT_MIN <= limit) {
        const struct jexfs3_dirent *de =
            (const struct jexfs3_dirent *)(block + pos);

        if (de->rec_len < JEXFS3_DIRENT_MIN || pos + de->rec_len > limit)
            return 0;
        if (de->name_len != 0 &&
            de->name_len <= de->rec_len - JEXFS3_DIRENT_MIN &&
            de->name_len == strlen(name) &&
            memcmp(de->name, name, de->name_len) == 0)
            return de->inode;
        pos += de->rec_len;
    }
    return 0;
}

static int read_file_meta(const uint8_t *img, uint32_t index,
                          uint64_t *size, uint32_t *crc)
{
    struct jexfs3_inode inode;

    if (inode_at(img, index, &inode) != 0)
        return -1;
    if ((inode.mode & JEXFS3_TYPE_MASK) != JEXFS3_TYPE_FILE)
        return -1;
    *size = inode.size;
    *crc = inode.data_crc;
    return 0;
}

/*
 * Write-crash scenario: fail the real writer after N block writes, then
 * replay the surviving journal exactly as a mount would.
 */
struct crash_call {
    uint8_t *img;
    uint32_t budget;
    uint32_t done;
};

static int crash_read_block(void *ctx, uint32_t block, void *buf)
{
    struct crash_call *call = (struct crash_call *)ctx;

    if (block >= total_blocks)
        return -1;
    memcpy(buf, call->img + (size_t)block * JEXFS3_BLOCK_SIZE,
           JEXFS3_BLOCK_SIZE);
    return 0;
}

static int crash_write_limited(void *ctx, uint32_t block, const void *buf)
{
    struct crash_call *call = (struct crash_call *)ctx;

    if (block >= total_blocks)
        return -1;
    if (call->budget == 0)
        return -1;
    call->budget--;
    call->done++;
    memcpy(call->img + (size_t)block * JEXFS3_BLOCK_SIZE, buf,
           JEXFS3_BLOCK_SIZE);
    return 0;
}

static int crash_image_write(void *ctx, uint32_t block, const void *buf)
{
    struct crash_call *call = (struct crash_call *)ctx;

    if (block >= total_blocks)
        return -1;
    memcpy(call->img + (size_t)block * JEXFS3_BLOCK_SIZE, buf,
           JEXFS3_BLOCK_SIZE);
    return 0;
}

static int crash_flush(void *ctx)
{
    (void)ctx;
    return 0;
}

static void *crash_alloc(size_t size)
{
    return malloc(size);
}

static void crash_free(void *ptr)
{
    free(ptr);
}

static uint8_t write_payload[3u * JEXFS3_BLOCK_SIZE + 123u];

static void check_write_case(uint32_t budget, uint32_t inode,
                             uint64_t old_size, uint32_t old_crc,
                             uint64_t new_size, uint32_t new_crc,
                             uint32_t *done_out)
{
    uint8_t *crash = (uint8_t *)malloc(image_size);
    struct crash_call call;
    struct jexfs3_write_io wio;
    struct jexfs3_journal_io jio;
    struct jexfs3_journal_info info;
    uint64_t size = 0;
    uint32_t crc = 0;
    char path[1024];
    int write_rc;
    int replay_rc;
    int meta_rc;
    int is_old;
    int is_new;

    if (!crash)
        die("out of memory for a write crash image");
    memcpy(crash, base, image_size);

    call.img = crash;
    call.budget = budget;
    call.done = 0;

    wio.read_block = crash_read_block;
    wio.write_block = crash_write_limited;
    wio.flush = crash_flush;
    wio.alloc = crash_alloc;
    wio.free = crash_free;
    wio.ctx = &call;

    write_rc = jexfs3_image_replace_file(&sb, &wio, inode, write_payload,
                                         sizeof(write_payload));
    if (done_out)
        *done_out = call.done;

    jio.read_block = crash_read_block;
    jio.write_block = crash_image_write;
    jio.flush = crash_flush;
    jio.ctx = &call;

    replay_rc = jexfs3_journal_replay(&sb, &jio, &info);
    meta_rc = read_file_meta(crash, inode, &size, &crc);
    is_old = meta_rc == 0 && size == old_size && crc == old_crc;
    is_new = meta_rc == 0 && size == new_size && crc == new_crc;

    cases++;
    if (replay_rc != 0) {
        fprintf(stderr, "  FAIL write budget=%u: replay returned %d\n",
                budget, replay_rc);
        failures++;
    } else if (meta_rc != 0) {
        fprintf(stderr, "  FAIL write budget=%u: inode unreadable after replay\n",
                budget);
        failures++;
    } else if (!is_old && !is_new) {
        fprintf(stderr, "  FAIL write budget=%u: inode is neither old nor new\n",
                budget);
        failures++;
    } else if (write_rc == 0 && !is_new) {
        fprintf(stderr, "  FAIL write budget=%u: success kept the old file\n",
                budget);
        failures++;
    }

    if (out_dir) {
        snprintf(path, sizeof(path), "%s/write-%03d-%05u.img",
                 out_dir, cases, budget);
        write_image(path, crash);
    }
    printf("  write budget %6u: %s%s\n", budget,
           is_new ? "new file" : "old file",
           write_rc == 0 ? " (complete)" : "");
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

    /*
     * Second scenario: crash the real file writer at every write
     * boundary.  The writer allocates blocks, writes data, logs the
     * bitmap+inode transaction and applies it.  A reboot replay must
     * leave each file exactly old or exactly new.
     *
     * data.bin also frees a depth-1 extent leaf in the transaction, so
     * the verifier can confirm both the data blocks and the leaf return
     * to the block bitmap.
     */
    {
        static const char *const names[] = { "hello.txt", "data.bin" };

        for (unsigned f = 0; f < sizeof(names) / sizeof(names[0]); f++) {
            uint32_t inode = find_file_inode(base, names[f]);
            uint64_t old_size;
            uint32_t old_crc;
            uint32_t new_crc;
            uint32_t total = 0;

            if (inode == 0)
                die("write test file not found in the image");
            if (read_file_meta(base, inode, &old_size, &old_crc) != 0)
                die("write test inode is unreadable");
            for (uint32_t i = 0; i < sizeof(write_payload); i++)
                write_payload[i] = (uint8_t)(0x5Au + i * 7u + f * 11u);
            new_crc = jexfs3_crc32c(0, write_payload, sizeof(write_payload));

            printf("j3crash: write path, %s inode %u, %llu -> %zu bytes\n",
                   names[f], inode, (unsigned long long)old_size,
                   sizeof(write_payload));
            check_write_case(UINT32_MAX, inode, old_size, old_crc,
                             sizeof(write_payload), new_crc, &total);
            printf("j3crash: write path uses %u block writes\n", total);
            for (uint32_t n = 0; n < total; n++)
                check_write_case(n, inode, old_size, old_crc,
                                 sizeof(write_payload), new_crc, NULL);
        }
    }

    printf("j3crash: %d case(s), %d failure(s)\n", cases, failures);
    free(txn_sb);
    free(base);
    free(logged);
    return failures ? 1 : 0;
}
