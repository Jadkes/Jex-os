/*
 * devtmpfs.c - Virtual filesystem backed by kernel callbacks
 *
 * Purpose: Provides a filesystem where every file is a thin shim over a
 *          kernel callback.  Reads invoke the file's .read() to produce
 *          content on the fly; writes invoke its .write() to handle input.
 *          Used for /sys/ inspection files and, in future, /dev/ nodes.
 *
 * Design:
 *   - Fixed-size entry table (DEVTMFS_MAX_FILES) indexed by add order.
 *   - Each entry stores a path + devtmpfs_file callback table.
 *   - open() walks the table for a path match, allocates a slot from
 *     the open-files table (DEVTMFS_MAX_OPEN), and returns a virtual FD
 *     (VIRTUAL_FD_START + slot index).
 *   - read()/write() call through to the entry's callback, honouring
 *     per-FD offset so sequential reads behave like regular files.
 *   - close() marks the slot free.
 *
 * Thread-safety: None yet -- the shell is single-threaded.
 */

#include "devtmpfs.h"
#include <string.h>

/* ------------------------------------------------------------------ */
/*  Constants                                                          */
/* ------------------------------------------------------------------ */

#define DEVTMFS_MAX_FILES  32
#define DEVTMFS_MAX_OPEN   8

/*
 * Virtual FD base -- must match VIRTUAL_FD_START in fs.c.
 * The VFS dispatch layer (fs.c) recognises fds >= this value as
 * belonging to a mounted virtual filesystem.
 */
#define VIRTUAL_FD_START   100

/* ------------------------------------------------------------------ */
/*  Entry table -- maps paths to callbacks                            */
/* ------------------------------------------------------------------ */

typedef struct {
    const char         *path;   /* Full virtual path, e.g. "/sys/kernel/version" */
    struct devtmpfs_file file;  /* Callback trio + metadata */
} devtmpfs_entry_t;

static devtmpfs_entry_t entries[DEVTMFS_MAX_FILES];
static int entry_count = 0;

/* ------------------------------------------------------------------ */
/*  Open-file table -- per-FD state                                   */
/* ------------------------------------------------------------------ */

typedef struct {
    int      used;       /* 1 = slot in use */
    int      entry_idx;  /* Index into entries[] */
    uint32_t offset;     /* Read cursor (bytes consumed so far) */
} devtmpfs_fd_t;

static devtmpfs_fd_t open_files[DEVTMFS_MAX_OPEN];

/* ------------------------------------------------------------------ */
/*  Public API -- called by filesystem drivers during boot             */
/* ------------------------------------------------------------------ */

/**
 * devtmpfs_add_file - Register a virtual file.
 * @path:  Full virtual path (e.g. "/sys/kernel/version").
 * @file:  Callback table.  The caller must keep the struct alive.
 * Return: 0 on success, -1 if the table is full.
 */
int devtmpfs_add_file(const char* path, struct devtmpfs_file* file)
{
    if (entry_count >= DEVTMFS_MAX_FILES)
        return -1;

    entries[entry_count].path     = path;
    entries[entry_count].file     = *file;
    entry_count++;
    return 0;
}

/**
 * devtmpfs_init - Prepare the filesystem for use.
 *
 * Currently a no-op -- the entry table is statically allocated and
 * cleared at load time.  Called once during boot.
 */
int devtmpfs_init(void)
{
    /* All tables are BSS, already zeroed by the bootloader. */
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Public API -- called by the VFS dispatch layer (fs.c)             */
/* ------------------------------------------------------------------ */

/**
 * devtmpfs_open - Open a virtual file by path.
 * @path:  Full virtual path.
 * @flags: Access flags (ignored for now).
 * Return: Virtual FD (>= VIRTUAL_FD_START) on success, -1 on failure.
 *
 * Scans the entry table for an exact path match, then allocates a slot
 * in the open-file table.
 */
int devtmpfs_open(const char* path, int flags)
{
    (void)flags;

    /* 1. Find the entry matching this path. */
    int idx = -1;
    for (int i = 0; i < entry_count; i++) {
        if (strcmp(path, entries[i].path) == 0) {
            idx = i;
            break;
        }
    }
    if (idx < 0)
        return -1;

    /* 2. Allocate a slot in the open-file table. */
    for (int i = 0; i < DEVTMFS_MAX_OPEN; i++) {
        if (!open_files[i].used) {
            open_files[i].used      = 1;
            open_files[i].entry_idx = idx;
            open_files[i].offset    = 0;
            return VIRTUAL_FD_START + i;
        }
    }

    return -1;  /* Too many open virtual files */
}

/**
 * devtmpfs_read - Read from an open virtual file.
 * @fd:   Virtual FD returned by devtmpfs_open().
 * @buf:  Destination buffer.
 * @size: Maximum number of bytes to read.
 * Return: Number of bytes read, 0 at EOF, or -1 on error.
 *
 * Calls the entry's read callback with a temporary stack buffer in case
 * the callback writes beyond @size.  Tracks offset within the FD so
 * sequential reads see continuous content.
 */
int devtmpfs_read(int fd, void* buf, uint32_t size)
{
    int slot = fd - VIRTUAL_FD_START;
    if (slot < 0 || slot >= DEVTMFS_MAX_OPEN || !open_files[slot].used)
        return -1;

    int idx = open_files[slot].entry_idx;
    if (!entries[idx].file.read)
        return -1;

    /*
     * Call the read callback with a stack buffer.  This lets the
     * callback always write a full response; we then copy out only
     * the bytes the caller asked for, respecting our per-FD offset.
     */
    char buf_local[256];
    int n = entries[idx].file.read(buf_local, (int)sizeof(buf_local));
    if (n <= 0)
        return 0;  /* EOF or empty */

    /* Skip bytes already consumed by earlier reads on this FD. */
    if ((int)open_files[slot].offset >= n)
        return 0;  /* EOF */

    int avail = n - (int)open_files[slot].offset;
    if ((int)size > avail)
        size = (uint32_t)avail;

    memcpy(buf, buf_local + open_files[slot].offset, size);
    open_files[slot].offset += size;
    return (int)size;
}

/**
 * devtmpfs_write - Write to an open virtual file.
 * @fd:   Virtual FD returned by devtmpfs_open().
 * @buf:  Source data.
 * @size: Number of bytes to write.
 * Return: @size on success, -1 on error.
 *
 * Calls the entry's write callback.  If the callback returns a non-zero
 * value (success), we count all bytes as written.
 */
int devtmpfs_write(int fd, const void* buf, uint32_t size)
{
    int slot = fd - VIRTUAL_FD_START;
    if (slot < 0 || slot >= DEVTMFS_MAX_OPEN || !open_files[slot].used)
        return -1;

    int idx = open_files[slot].entry_idx;
    if (!entries[idx].file.write)
        return -1;

    if (entries[idx].file.write((const char*)buf, (int)size))
        return (int)size;
    return -1;
}

/**
 * devtmpfs_close - Close an open virtual file.
 * @fd: Virtual FD to close.
 * Return: 0 on success, -1 on error.
 */
int devtmpfs_close(int fd)
{
    int slot = fd - VIRTUAL_FD_START;
    if (slot < 0 || slot >= DEVTMFS_MAX_OPEN)
        return -1;

    open_files[slot].used = 0;
    return 0;
}
