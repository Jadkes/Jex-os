# JexFS v3 on-disk format

JexFS v3 is the format break behind the v2 ceiling: 32-bit block numbers
and 4 KB blocks (16 TiB) instead of 16-bit block numbers and 1 KB blocks
(64 MB), 64-bit file sizes instead of 32-bit, extents instead of direct +
indirect block pointers, and crc32c integrity over metadata.

Phase 1 implements the format, a host builder (`mkjexfs3`), a host
verifier (`jexfsck3`), and a read-only kernel mount.  Phase 2a adds the
metadata journal: its on-disk format, a shared replay path used by the
kernel and the host verifier, and deterministic crash simulation.  Phase
2b adds the writable kernel path: block allocation, ordered data writes
and a journaled metadata transaction that replaces an existing file.
Phase 2c adds file creation and unlinking, including inode allocation and
the free-entry form of a directory entry.  Images may still carry no
journal, in which case they remain read-only.

## Block layout

Block numbers are `uint32_t`, block size is 4096 bytes, and every multi-byte
field is little-endian.

| Block(s) | Contents |
|---|---|
| 0 | Primary superblock |
| 1 .. `inode_bitmap_blocks` | Inode bitmap, 1 bit per inode |
| .. `block_bitmap_blocks` | Block bitmap, 1 bit per block |
| .. `inode_table_blocks` | Inode table, 256-byte records |
| .. `journal_blocks` | Journal, when the image has one |
| `first_data_block` .. `backup_block - 1` | File data and extent leaf blocks |
| `backup_block` (last block) | Backup superblock, byte-identical to block 0 |

The journal sits between the inode table and the first data block, so
`first_data_block = inode_table_start + inode_table_blocks + journal_blocks`.
Reserving it at build time keeps the journal out of the allocator's way
and means a torn transaction can only damage blocks the journal owns.

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
| 40 | `generation` | Bumped when the superblock itself changes |
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
| 88 | `first_data_block` | First block after the inode table and journal |
| 92 | `journal_start` | First journal block, 0 when there is no journal |
| 96 | `journal_blocks` | Journal length in blocks, 0 when there is no journal |
| 100 | `root_inode` | 2 |
| 104 | `checksum_type` | 1 = crc32c |
| 108 | `checksum` | crc32c over the full 4 KB block, this field zeroed |
| 112 | `backup_block` | `total_blocks - 1` |
| 116 | `backup_generation` | Generation of the backup copy |
| 120 | `label[32]` | Human-readable label |

Required incompat bits are `EXTENTS` (0x1) and `CRC32C` (0x2).
`JOURNAL` (0x4) is set exactly when `journal_blocks != 0`; a set bit
without a region, or a region without the bit, is an invalid superblock.
A mount also requires the metadata layout to match the block counts
implied by `inode_count`, `total_blocks` and `journal_blocks`, and the
backup superblock to match the primary byte-for-byte.

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

A dirent with `inode == 0` is free space.  It keeps its `rec_len`, so the
chain still covers its bytes, but `name_len` and `file_type` are zero and
readers skip it.  Unlinking turns an entry into this form instead of
shifting later entries or shrinking the directory, which keeps deletion
to one block image and one journal transaction.  Creating a file reuses a
free entry whole when it fits, or splits the unused tail of an existing
entry.

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

## Journal

The journal is one metadata transaction slot, deliberately not an ext4
log.  There is no circular buffer, no checkpointing, no revoke records
and no multi-transaction state machine: a transaction either has a valid
commit block or it is ignored.

```
journal_start:      header     (immutable, written at format time)
journal_start + 1:  descriptor (magic "J3DS", count, target[count])
journal_start + 2:  data block 0
                    ...
                    data block count-1
                    commit     (magic "J3CM", count, crc32c)
```

The header records the journal magic (`0x4E4A334A`), version 1, block
size, length and a crc32c at offset 16.  It never changes after format,
so a torn transaction cannot lose the record of where the journal is.

The descriptor carries up to 64 target block numbers.  Its targets must
be inside the image, outside the journal itself, and unique; duplicates
or a count above `journal_blocks - 3` are rejected.  The commit block's
crc32c covers the descriptor block and every data block, so a commit
proves the whole transaction reached the disk.

Replay rules:

- validate the header, descriptor magic, target list and commit crc;
- a missing or invalid commit abandons the transaction without writing
  any target, so the old on-disk state survives;
- a valid commit applies each data block to its target, flushes, then
  clears the descriptor magic;
- applying the same transaction twice is harmless, because a data block
  is a whole block image rather than a delta.  A crash between the apply
  and the clear only repeats the same writes on the next mount.

File data is never journaled.  The writer follows ordered data: write the
new data blocks to their final homes and flush, then write the
transaction (descriptor, data, commit) and only then apply the metadata.
That is enough for this filesystem because a replayed inode must never
point at data that had not reached the disk.

A committed transaction on a read-only device is a mount failure
(`-EROFS`), not a silent mount with stale metadata.  A journal with no
committed transaction does not require write access at all.

## Writing files

Phase 2b replaces the contents of an existing regular file; phase 2c
creates and unlinks them.  Offset writes, rename, directory creation and
truncate-by-offset are still ahead; a zero-length replacement is a
truncate.

The write path lives in `src/fs/jexfs3_write.c` and is shared by the
kernel and the host tools.  A replacement:

1. validates the old inode and its extent list;
2. allocates a fresh contiguous run from the block bitmap, never a block
   the old inode still references;
3. writes and zero-pads the new data blocks, then flushes them;
4. builds the new inode (one contiguous inline extent chain, split into
   65535-block pieces) and the modified block-bitmap block(s);
5. logs the changed bitmap block(s) and inode as one transaction, then
   applies and clears it.

New data goes to fresh blocks so a crash before the commit leaves the old
inode, old blocks and old data intact.  A crash after the commit replays
the bitmap and inode together, so the new file appears complete.  Blocks
allocated by a write that never commits stay marked free: their contents
are garbage, but no inode references them.  One consequence is that a
rewrite needs free space for the whole new file; the old blocks are not
available to the allocator until the transaction commits.  Delayed
allocation in phase 3 can lift that constraint.

The replacement is contiguous, so it needs no extent leaf.  A file that
would need more than four inline extents (more than 262140 blocks) is
rejected with `-EFBIG`; that limit is far above the test images.  A file
written this way can free an old leaf extent block, because the old
extent list is walked and every block it names returns to the bitmap in
the same transaction.

Creation and unlinking share the same staging rules:

- Creating a file picks the lowest free inode, writes a zero-length inode
  record, claims its inode-bitmap bit and adds a dirent to the parent,
  then recomputes the parent's data crc.  All of those blocks are one
  transaction, so a crash leaves the file absent or complete.
- Unlinking finds the dirent, checks the inode is an unshared regular
  file, turns the dirent into a free entry, returns the file's data and
  extent blocks to the block bitmap, clears the inode-bitmap bit and
  updates the parent crc, again in one transaction.  The inode record is
  left on disk; a clear bitmap bit means it is not in use.
- The parent directory must already have a free entry or enough unused
  tail in an existing entry.  Growing a directory (allocating data
  blocks and updating its extent list) is not implemented yet, so a full
  directory fails with `-ENOSPC` rather than being resized.

Both operations stage every metadata block they touch and commit them
together, so the inode bitmap, block bitmap, inode records and directory
cannot disagree after a crash.  The journal can hold 64 data blocks; an
operation that would need more (a very wide block bitmap or a parent
directory in another inode-table block) fails with `-ENOSPC`.

## Host tools

```bash
./build/tools/mkjexfs3 <out.img> <blocks> [--split=N] [--journal=N] <dir...>
./build/tools/jexfsck3 <image.img> [--replay] [dir...]
./build/tools/j3crash <image.img> [--out DIR]
```

`mkjexfs3` merges one or more source directories into the image root and
packs regular files and directories.  `--split=N` caps each extent at N
blocks, which is how the test image gets a file with five extents and a
leaf block; production images leave it unset so files are one extent when
possible.  `--journal=N` reserves N blocks (0, or at least 4) and writes
an empty journal header.  The builder refuses files that need more than
255 extents (a deeper tree is phase 3).  It reserves 16 spare inodes past
the packed tree, with their bitmap bits clear, so a freshly built image
can accept new files without being rebuilt.

`jexfsck3` validates both superblocks, the layout, every used inode and
extent checksum, coverage and bounds, directory chains and link counts,
the inode and block bitmaps, and each file's data crc.  Given the same
source directories, it also compares every file byte-for-byte and reports
missing or extra entries.  A pending transaction is reported, not
applied, unless `--replay` is given; replay calls the same code the
kernel uses at mount.

`j3crash` runs three scenarios.  First it logs a two-block transaction
(a generation bump written to both superblock copies), then replays every
truncation point of it against a scratch copy.  Each prefix stands for a
crash boundary; every resulting image must hold either the old generation
or the new one, never a torn pair, and must keep a valid superblock
checksum.  One extra case commits the journal and then applies only the
first target, which is the other crash window; replay must finish the
transaction and clear the descriptor.

The second scenario crashes the real writer: it calls
`jexfs3_image_replace_file()` with a callback that fails after every
possible number of block writes, then replays the surviving journal as a
mount would.  The target file must be exactly old or exactly new.  It
does this for an inline-extent file and for a file that frees a leaf
extent block, so both the data blocks and the leaf are checked against
the block bitmap by `jexfsck3`.

The third scenario does the same for `jexfs3_image_create_file()` and
`jexfs3_image_unlink_file()`.  A crashed create must leave no dirent or
an empty file; a crashed unlink must keep the file with its old contents
or remove it completely.  The verifier then checks the inode and block
bitmaps and the directory chain of every resulting image.

`make check-v3` builds the test tree and image and runs the verifier.
`make crash-sim` runs `j3crash`, writes all 91 crash images, and runs the
full verifier with `--replay` over each one.
`make FLAVOR=test run-test` packs two images into the v2 test rootfs:
`/jexfs3.img` (the extent test) and `/jexfs3-journal.img` (a 48-block
image with an 8-block journal).  The kernel test mounts the first image,
then copies the second into a RAM buffer so it can log a transaction, tear
off its commit block, mount (must ignore it), restore the commit, mount
again (must replay it exactly once), and finally confirm that a pending
transaction on a read-only device fails with `-EROFS`.  A further test
creates two files, writes one through `jexfs3_replace_file()`, unlinks
it, walks the directory (the free entry must not appear), remounts and
confirms the surviving file, and checks that a read-only device refuses
create and unlink.

## Kernel API

`src/include/jexfs3_fs.h` is the driver interface:

- `jexfs3_mount(dev)` / `jexfs3_unmount()` / `jexfs3_mounted()`
- `jexfs3_superblock(out)`
- `jexfs3_lookup(path, &inode)`
- `jexfs3_read_file(inode, buf, size, offset)`
- `jexfs3_replace_file(inode, buf, size)`
- `jexfs3_create_file(path, &inode)`
- `jexfs3_unlink_file(path)`
- `jexfs3_readdir(inode, &cookie, name, cap, &child, &type)`
- `jexfs3_verify()`

The device supplies `read_block(ctx, block, buf)`, plus optional
`write_block` and `flush` callbacks for journal replay and file writes.
A single 4 KB static cache keeps metadata parsing off the 8 KB kernel
stack, and a mutex makes the cache safe for concurrent readers and
serialises writers; a write invalidates the cache afterwards.  Mount
validates both superblocks, replays a committed transaction (re-reading
and revalidating the superblock afterwards, since a transaction can
rewrite it), then checks the root inode.  `jexfs3_verify()` walks every
allocated inode, recomputes its data crc, checks dirent chains and child
types, and confirms every allocated inode is reachable from the root.

`src/include/jexfs3_journal.h` exposes the shared journal API
(`check`, `replay`, `write`, `clear`) used by the kernel, `jexfsck3` and
`j3crash`.  `src/include/jexfs3_write.h` exposes the shared image writer
(`jexfs3_image_replace_file`, `jexfs3_image_create_file`,
`jexfs3_image_unlink_file`) used by the kernel wrapper and `j3crash`.
One implementation of each rule means the verifier cannot accept a
transaction the kernel would reject.

## Phase status

| Phase | Scope | Status |
|---|---|---|
| 0 | Disk-first v2 mount, module pages returned to the PMM | done |
| 1 | Format, `mkjexfs3`, `jexfsck3`, read-only kernel mount | done |
| 2a | Journal format, shared replay, deterministic crash simulation | done |
| 2b | Writable kernel path (allocation, metadata transactions) | done |
| 2c | File lifecycle: create and unlink | done |
| 3 | Extent tree, delayed allocation, hashed directories | planned |
| 4 | v2→v3 converter, persistence rewrite | planned |

Phase 2b replaces existing regular files and phase 2c creates and
unlinks them.  Renaming files, offset writes, directory creation and
removal, symbolic links and hard links are still ahead; the shared
writer is the place those operations will build on.
