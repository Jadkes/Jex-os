/**
 * @file jexfs3.h
 * @brief JexFS v3 on-disk format -- shared by the kernel and host tools.
 *
 * Layout (4 KB blocks, u32 block numbers):
 *   Block 0:            Primary superblock
 *   Blocks 1..:         Inode bitmap     (1 bit per inode)
 *   Blocks ..:          Block bitmap     (1 bit per block)
 *   Blocks ..:          Inode table      (256-byte inodes)
 *   Blocks ..:          Journal          (optional, journal_blocks long)
 *   Blocks ..:          Data / extent blocks
 *   Last block:         Backup superblock
 *
 * Every superblock, inode and extent block carries a crc32c.  File and
 * directory data are covered by the inode's data_crc.  The on-disk byte
 * order is little-endian; both JexOS and the build host are x86.
 *
 * Extents: an inode holds up to four inline extents.  A file needing more
 * gets one leaf block (depth 1) holding up to 255 extents.  Deeper trees
 * are reserved for a later phase; the format keeps depth explicit so they
 * can be added without another version break.
 */

#ifndef JEXFS3_H
#define JEXFS3_H

#include <stdint.h>
#include <stddef.h>

#define JEXFS3_MAGIC              0x3353464Au  /* "JFS3" on disk */
#define JEXFS3_EXTENT_MAGIC       0x5845334Au  /* "J3EX" on disk */
#define JEXFS3_VERSION            3u
#define JEXFS3_BLOCK_SIZE         4096u
#define JEXFS3_INODE_SIZE         256u
#define JEXFS3_INLINE_EXTENTS     4u
#define JEXFS3_EXTENT_ENTRIES     255u
#define JEXFS3_DIRENT_MIN         8u
#define JEXFS3_ROOT_INODE         2u
#define JEXFS3_NAME_MAX           255u

/* Feature flags.  An unknown incompat bit means "do not mount". */
#define JEXFS3_FEATURE_INCOMPAT_EXTENTS 0x00000001u
#define JEXFS3_FEATURE_INCOMPAT_CRC32C  0x00000002u
#define JEXFS3_FEATURE_INCOMPAT_JOURNAL 0x00000004u
#define JEXFS3_FEATURE_INCOMPAT_KNOWN \
    (JEXFS3_FEATURE_INCOMPAT_EXTENTS | JEXFS3_FEATURE_INCOMPAT_CRC32C | \
     JEXFS3_FEATURE_INCOMPAT_JOURNAL)

/* Inode mode: POSIX-style type nibble plus permission bits. */
#define JEXFS3_TYPE_MASK          0xF000u
#define JEXFS3_TYPE_FILE          0x8000u
#define JEXFS3_TYPE_DIR           0x4000u
#define JEXFS3_IRUSR              0x0100u
#define JEXFS3_IWUSR              0x0080u
#define JEXFS3_IXUSR              0x0040u
#define JEXFS3_IRGRP              0x0020u
#define JEXFS3_IWGRP              0x0010u
#define JEXFS3_IXGRP              0x0008u
#define JEXFS3_IROTH              0x0004u
#define JEXFS3_IWOTH              0x0002u
#define JEXFS3_IXOTH              0x0001u

#define JEXFS3_DIRENT_FILE        1u
#define JEXFS3_DIRENT_DIR         2u

/* Offsets of the checksum fields inside their structs. */
#define JEXFS3_SB_CHECKSUM_OFF    108u
#define JEXFS3_INODE_CHECKSUM_OFF 120u
#define JEXFS3_EXTENT_CHECKSUM_OFF 8u

struct jexfs3_extent {
    uint32_t logical;      /* first file block described */
    uint32_t physical;     /* first on-disk block */
    uint16_t length;       /* blocks covered, 1..65535 */
    uint16_t flags;        /* reserved, must be 0 */
    uint32_t reserved;
} __attribute__((packed));

struct jexfs3_superblock {
    uint32_t magic;
    uint32_t version;
    uint32_t block_size;
    uint32_t total_blocks;
    uint64_t total_bytes;
    uint8_t  uuid[16];
    uint32_t generation;
    uint32_t feature_compat;
    uint32_t feature_ro_compat;
    uint32_t feature_incompat;
    uint32_t inode_count;
    uint32_t inode_size;
    uint32_t inode_bitmap_start;
    uint32_t inode_bitmap_blocks;
    uint32_t block_bitmap_start;
    uint32_t block_bitmap_blocks;
    uint32_t inode_table_start;
    uint32_t inode_table_blocks;
    uint32_t first_data_block;
    uint32_t journal_start;      /* 0 when the image has no journal */
    uint32_t journal_blocks;
    uint32_t root_inode;
    uint32_t checksum_type;      /* 1 = crc32c */
    uint32_t checksum;
    uint32_t backup_block;
    uint32_t backup_generation;
    uint8_t  label[32];
    uint8_t  reserved[104];
} __attribute__((packed));

struct jexfs3_inode {
    uint16_t mode;
    uint16_t links;
    uint16_t uid;
    uint16_t gid;
    uint32_t flags;
    uint32_t generation;
    uint64_t size;
    uint64_t blocks;             /* allocated 4 KB blocks */
    uint32_t data_crc;           /* crc32c over size bytes of data */
    uint32_t atime;
    uint32_t ctime;
    uint32_t mtime;
    uint16_t extent_count;       /* inline extents, or entries in root */
    uint16_t extent_depth;       /* 0 = inline only, 1 = one leaf level */
    uint32_t extent_root;        /* leaf block, 0 when inline */
    struct jexfs3_extent inline_extents[JEXFS3_INLINE_EXTENTS];
    uint32_t checksum;
    uint8_t  reserved[132];
} __attribute__((packed));

struct jexfs3_extent_block {
    uint32_t magic;
    uint16_t depth;
    uint16_t count;
    uint32_t checksum;
    uint32_t reserved;
    struct jexfs3_extent entries[JEXFS3_EXTENT_ENTRIES];
} __attribute__((packed));

struct jexfs3_dirent {
    uint32_t inode;
    uint16_t rec_len;
    uint8_t  name_len;
    uint8_t  file_type;
    char     name[];
} __attribute__((packed));

/*
 * Journal -- one metadata transaction slot, not a circular ext4 log.
 *
 * A transaction is laid out at journal_start + 1:
 *
 *   descriptor block | data block 0 .. data block N-1 | commit block
 *
 * The descriptor lists the target block of each data block.  The commit
 * block carries a crc32c over the descriptor and every data block, so a
 * transaction is valid only when all of it reached the disk.  Replay
 * applies the committed transaction and ignores one with a missing or
 * bad commit.
 *
 * Writing a transaction twice is harmless: the data blocks hold whole
 * block images, not deltas.  After applying a transaction the writer
 * clears the descriptor magic; a crash before that clear only makes the
 * next mount replay the same blocks again.
 *
 * File data is never journaled.  The writer must write data blocks to
 * their final location and flush before writing the commit block, which
 * is the ordered-data rule that keeps a replayed inode from pointing at
 * stale data.
 */
#define JEXFS3_JOURNAL_MAGIC        0x4E4A334Au  /* "J3JN" on disk */
#define JEXFS3_JOURNAL_DESC_MAGIC   0x534A334Au  /* "J3DS" on disk */
#define JEXFS3_JOURNAL_COMMIT_MAGIC 0x434A334Au  /* "J3CM" on disk */
#define JEXFS3_JOURNAL_VERSION      1u
#define JEXFS3_JOURNAL_MIN_BLOCKS   4u   /* header + desc + one data + commit */
#define JEXFS3_JOURNAL_MAX_TXN      64u

/* Offsets of the checksum fields inside their structs. */
#define JEXFS3_JOURNAL_HEADER_CHECKSUM_OFF 16u
#define JEXFS3_JOURNAL_COMMIT_CHECKSUM_OFF 8u

struct jexfs3_journal_header {
    uint32_t magic;
    uint32_t version;
    uint32_t block_size;
    uint32_t journal_blocks;
    uint32_t checksum;
} __attribute__((packed));

struct jexfs3_journal_desc {
    uint32_t magic;
    uint32_t count;              /* data blocks, 1..journal_blocks - 3 */
    uint32_t targets[JEXFS3_JOURNAL_MAX_TXN];
} __attribute__((packed));

struct jexfs3_journal_commit {
    uint32_t magic;
    uint32_t count;
    uint32_t checksum;           /* crc over desc + every data block */
    uint32_t reserved;
} __attribute__((packed));

/**
 * jexfs3_crc32c - Castagnoli CRC used on every metadata block.
 *
 * The 256-entry table is built on first use.  Mount and verification are
 * single-threaded; there is no locking here.
 *
 * @param crc Initial value (0 for a new checksum)
 * @param buf Data to hash
 * @param len Byte count
 * @return Updated CRC
 */
static inline uint32_t jexfs3_crc32c(uint32_t crc, const void *buf, uint32_t len)
{
    static uint32_t table[256];
    static int table_ready;
    const uint8_t *p = (const uint8_t *)buf;

    if (!table_ready) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t v = i;
            for (int bit = 0; bit < 8; bit++)
                v = (v & 1u) ? (v >> 1) ^ 0x82F63B78u : v >> 1;
            table[i] = v;
        }
        table_ready = 1;
    }

    crc = ~crc;
    while (len--)
        crc = table[(crc ^ *p++) & 0xFFu] ^ (crc >> 8);
    return ~crc;
}

/**
 * jexfs3_crc_zeroed - CRC over a buffer with a checksum field read as zero.
 *
 * Computing over the field's zero image (instead of storing zero, hashing,
 * then restoring) keeps readers from modifying data they only verify.
 *
 * @param buf Buffer start
 * @param len Buffer length
 * @param off Offset of the 4-byte field to treat as zero
 * @return crc32c over the buffer
 */
static inline uint32_t jexfs3_crc_zeroed(const void *buf, uint32_t len,
                                         uint32_t off)
{
    const uint8_t *p = (const uint8_t *)buf;
    uint32_t zero = 0;
    uint32_t crc;

    crc = jexfs3_crc32c(0, p, off);
    crc = jexfs3_crc32c(crc, &zero, 4);
    return jexfs3_crc32c(crc, p + off + 4, len - off - 4);
}

#endif /* JEXFS3_H */
