# JexFS v3 on-disk format

JexFS v3 is the format break behind the v2 ceiling: 32-bit block numbers
and 4 KB blocks (16 TiB) instead of 16-bit block numbers and 1 KB blocks
(64 MB), 64-bit file sizes instead of 32-bit, extents instead of direct +
indirect block pointers, and crc32c integrity over metadata.

Phase 1 implements the format, a host builder (`mkjexfs3`), a host
verifier (`jexfsck3`), and a read-only kernel mount.  The journal is the
next phase; images built now declare `journal_blocks = 0` and the kernel
refuses to mount one that claims otherwise.

## Block layout

Block numbers are `uint32_t`, block size is 4096 bytes, and every multi-byte
field is little-endian.

| Block(s) | Contents |
|---|---|
| 0 | Primary superblock |
| 1 .. `inode_bitmap_blocks` | Inode bitmap, 1 bit per inode |
| .. `block_bitmap_blocks` | Block bitmap, 1 bit per block |
| .. `inode_table_blocks` | Inode table, 256-byte records |
| `first_data_block` .. `backup_block - 1` | File data and extent leaf blocks |
| `backup_block` (last block) | Backup superblock, byte-identical to block 0 |

Inode numbers 0 and 1 are reserved and are never allocated; the root is
inode 2.  Their bitmap bits stay clear, so a scanner never expects inode
records that were never written.

## Superblock (256 bytes)

| Offset | Field | Notes |
|---:|---|---|
| 0 | `magic` | `0x3353464A` ("JFS3" little-endian) |
| 4 | `version` | 3 |
| 8 | `block_size` | 4096 |
| 12 | `total_blocks` | Device size in blocks |
| 16 | `total_bytes` | `total_blocks * block_size` |
| 24 | `uuid[16]` | Name-based, derived from the packed tree |
| 40 | `generation` | Bumped by every metadata transaction |
| 44 | `feature_compat` | Read/write-compatible features (0 today) |
| 48 | `feature_ro_compat` | Read-only-compatible features (0 today) |
| 52 | `feature_incompat` | Unknown bit means "do not mount" |
| 56 | `inode_count` | Inode records in the table |
| 60 | `inode_size` | 256 |
| 64 | `inode_bitmap_start` | 1 |
| 68 | `inode_bitmap_blocks` | `ceil(inode_count / 32768)` |
| 72 | `block_bitmap_start` | Follows the inode bitmap |
| 76 | `block_bitmap_blocks` | `ceil(total_blocks / 32768)` |
| 80 | `inode_table_start` | Follows the block bitmap |
| 84 | `inode_table_blocks` | `ceil(inode_count * 256 / 4096)` |
| 88 | `first_data_block` | First block after the inode table |
| 92 | `journal_start` | 0 until phase 2 |
| 96 | `journal_blocks` | 0 until phase 2 |
| 100 | `root_inode` | 2 |
| 104 | `checksum_type` | 1 = crc32c |
| 108 | `checksum` | crc32c over the full 4 KB block, this field zeroed |
| 112 | `backup_block` | `total_blocks - 1` |
| 116 | `backup_generation` | Generation of the backup copy |
| 120 | `label[32]` | Human-readable label |

Required incompat bits in phase 1 are `EXTENTS` (0x1) and `CRC32C` (0x2).
A mount also requires the metadata layout to match the block counts
implied by `inode_count` and `total_blocks`, and the backup superblock to
match the primary byte-for-byte.

## Inode (256 bytes)

| Offset | Field | Notes |
|---:|---|---|
| 0 | `mode` | POSIX type nibble (`0x8000` file, `0x4000` dir) + permissions |
| 2 | `links` | Files: 1.  Directories: 2 + subdirectories |
| 4 | `uid`, `gid` | 16 bits each |
| 8 | `flags` | Reserved, 0 |
| 12 | `generation` | Per-inode generation |
| 16 | `size` | File or directory size in bytes (64-bit) |
| 24 | `blocks` | Allocated 4 KB blocks, including a leaf extent block |
| 32 | `data_crc` | crc32c over exactly `size` bytes of file/directory data |
| 36,40,44 | `atime`, `ctime`, `mtime` | Seconds |
| 48 | `extent_count` | Inline extents, or entries in the root leaf |
| 50 | `extent_depth` | 0 = inline only, 1 = one leaf level |
| 52 | `extent_root` | Leaf block number, 0 when inline |
| 56 | `inline_extents[4]` | Four 16-byte extents |
| 120 | `checksum` | crc32c over the 256-byte inode, this field zeroed |
| 124 | `reserved[132]` | Zero |

## Extents

```
struct jexfs3_extent {
    uint32_t logical;    /* first file block */
    uint32_t physical;   /* first on-disk block */
    uint16_t length;     /* blocks covered (1..65535) */
    uint16_t flags;      /* must be 0 */
    uint32_t reserved;
};
```

An inode holds up to four inline extents.  A file needing more gets one
leaf block holding up to 255 entries; `extent_depth` is 1 and
`extent_root` names that block.  Deeper trees are reserved for phase 3,
and the reader rejects a depth it does not know.

The extent list must:
- start at logical block 0 and be contiguous with no holes or overlaps,
- cover exactly `ceil(size / 4096)` blocks,
- point at or above `first_data_block` and end below `backup_block`,
- account for `inode->blocks` as data blocks plus one when a leaf exists.

An extent leaf block is:

```
magic (0x5845334A "J3EX"), depth (1), count, checksum at offset 8,
reserved, then 255 extent entries
```

## Directories

```
struct jexfs3_dirent {
    uint32_t inode;
    uint16_t rec_len;    /* distance to the next entry */
    uint8_t  name_len;
    uint8_t  file_type;  /* 1 = file, 2 = directory */
    char     name[];
};
```

Entries form a `rec_len` chain through each directory block.  The last
entry in a block is stretched to the block boundary, and directory size
is normally a whole number of blocks.  Every directory starts with `.`
(pointing at itself) and `..` (pointing at its parent; the root points at
itself).  Names are 1..255 bytes and are not NUL-terminated on disk.

## Checksums

All checksums are Castagnoli crc32c (`0x82F63B78` reflected polynomial):

- Superblock: crc32c over the full 4096-byte block with the `checksum`
  field (offset 108) read as zero.  The backup block carries the same
  checksum because it is an identical byte image.
- Inode: crc32c over the 256-byte record with `checksum` (offset 120)
  read as zero.
- Extent leaf: crc32c over the 4096-byte block with `checksum` (offset 8)
  read as zero.
- File and directory data: crc32c over exactly `size` bytes, stored in
  `data_crc`.  Unlike metadata it is not stored in-band, so a data block
  can be verified without trusting the block's contents.

`jexfs3_crc_zeroed()` hashes the zero image of a field without modifying
the buffer, so verifiers never write to media they are only checking.

## Host tools

```bash
./build/tools/mkjexfs3 <out.img> <blocks> [--split=N] <dir...>
./build/tools/jexfsck3 <image.img> [dir...]
```

`mkjexfs3` merges one or more source directories into the image root and
packs regular files and directories.  `--split=N` caps each extent at N
blocks, which is how the test image gets a file with five extents and a
leaf block; production images leave it unset so files are one extent when
possible.  The builder refuses files that need more than 255 extents (a
deeper tree is phase 3).

`jexfsck3` validates both superblocks, the layout, every used inode and
extent checksum, coverage and bounds, directory chains and link counts,
the inode and block bitmaps, and each file's data crc.  Given the same
source directories, it also compares every file byte-for-byte and reports
missing or extra entries.

`make check-v3` builds the test tree and image and runs the verifier.
`make FLAVOR=test run-test` packs that image into the v2 test rootfs as
`/jexfs3.img`; the kernel test mounts it, runs `jexfs3_verify()`, and
reads files across an extent boundary.

## Kernel API

`src/include/jexfs3_fs.h` is the read-only driver interface:

- `jexfs3_mount(dev)` / `jexfs3_unmount()` / `jexfs3_mounted()`
- `jexfs3_superblock(out)`
- `jexfs3_lookup(path, &inode)`
- `jexfs3_read_file(inode, buf, size, offset)`
- `jexfs3_readdir(inode, &cookie, name, cap, &child, &type)`
- `jexfs3_verify()`

The device supplies a `read_block(ctx, block, buf)` callback, so the same
driver can later serve an IDE partition.  A single 4 KB static cache
keeps metadata parsing off the 8 KB kernel stack, and a mutex makes the
cache safe for concurrent readers.  Mount verifies both superblocks and
the root inode; `jexfs3_verify()` walks every allocated inode, recomputes
its data crc, checks dirent chains and child types, and confirms every
allocated inode is reachable from the root.

## Phase status

| Phase | Scope | Status |
|---|---|---|
| 0 | Disk-first v2 mount, module pages returned to the PMM | done |
| 1 | Format, `mkjexfs3`, `jexfsck3`, read-only kernel mount | done |
| 2 | Metadata journal, replay, ordered data, crash injection | next |
| 3 | Extent tree, delayed allocation, hashed directories | planned |
| 4 | v2→v3 converter, persistence rewrite | planned |

Phase 1 is read-only by design.  Writes, allocation, and journal replay
belong to phases 2–4, and the kernel rejects an image that claims a
journal rather than mounting it without recovery.
