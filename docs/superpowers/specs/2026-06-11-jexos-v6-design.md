# JexOS v6.0: Infrastructure & Maturity

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this spec task-by-task.

**Goal:** Replace the bump allocator with a power-of-2 slab allocator that can actually free memory, add signal handling, add keyboard buffering, and fix bugs that were impossible to fix under the old allocator.

**Architecture:** Implement a slab allocator with 9 fixed-size classes (16B–4096B), slab metadata embedded at page boundaries for O(1) kfree, and PMM fallback for allocations > 4096 bytes. On top of this, add minimal Unix-style signals (SIGHUP/SIGINT/SIGTERM/SIGKILL/SIGCHLD) and a 256-byte keyboard ring buffer.

**Tech Stack:** Freestanding C (gnu99), target i386, runs in QEMU. No libc — all string functions are in kheap.c and stay there.

---

## 1. Slab Allocator

### Size Classes

| Class | Size | Objects/slab | Typical uses |
|-------|------|--------------|-------------|
| 0 | 16 | 256 | Tiny structs, flags |
| 1 | 32 | 128 | `file_descriptor_t`, token_t in TCC |
| 2 | 64 | 64 | Small allocations |
| 3 | 128 | 32 | `task_t` (currently 88 bytes), `tcc_state_t` |
| 4 | 256 | 16 | `eth_header_t+payload`, small buffers |
| 5 | 512 | 8 | Medium strings, path buffers |
| 6 | 1024 | 4 | File data chunks |
| 7 | 2048 | 2 | `PACKET_BUF_SIZE` (2048) |
| 8 | 4096 | 1 | Source code buffers, ram_disk (via PMM) |

### Slab Header (at page start, 4KB-aligned)

```c
#define SLAB_MAGIC 0x534C4142  /* "SLAB" */
#define LARGE_MAGIC 0x4C524700 /* "LRG\0" */

typedef struct slab {
    uint32_t   magic;           /* SLAB_MAGIC or LARGE_MAGIC */
    struct slab* next;          /* Next slab in this cache's list */
    uint32_t   obj_size;        /* Size of each object */
    uint32_t   obj_count;       /* Total objects in this slab */
    uint32_t   free_count;      /* Number of free objects */
    uint32_t   free_head;       /* Index of first free object */
    uint32_t   pad;             /* 8-byte align next field */
} slab_t;                      /* 28 bytes total */
```

Slab header + first object = 28 + 16 = 44 bytes. In class 0 (16B objects): 256 objects × 16 = 4096, minus header → 4068 bytes used → ~99% space efficiency. In class 7 (2048B objects): 2 × 2048 = 4096, minus 28 → 4068 → full 2-object slab.

### Intra-slab Free List

Free objects store the index of the next free object as a `uint32_t` at offset 0. A value of `0xFFFFFFFF` means "end of list." The `free_head` field in the header holds the index of the first free object.

```
Allocation:  obj_idx = head; head = *(uint32_t*)(slab_data + head * obj_size); free_count--
Deallocation: *(uint32_t*)(slab_data + idx * obj_size) = head; head = idx; free_count++
```

No external bitmap. No O(n) search. O(1) alloc/free.

### Page Recovery

`kfree(ptr)` does:
1. `slab = (slab_t*)((uint32_t)ptr & ~0xFFF)` — page-align
2. If `slab->magic == SLAB_MAGIC`: standard slab free
3. If `slab->magic == LARGE_MAGIC`: the pointer was a PMM-backed large alloc. Read the total size from the header, free the PMM blocks.
4. Else: silent no-op (defensive — print warning on debug builds)

After free: if `free_count == obj_count`, all objects in the slab are free. The slab is returned to the cache's free list. A low-priority reaper (or explicit `slab_reclaim()` call) can return completely-free slabs back to the PMM for reuse.

### Cache Structure

```c
typedef struct {
    slab_t*    slab_list;       /* Linked list of all slabs in this cache */
    uint32_t   obj_size;        /* Object size for this cache */
    uint32_t   objs_per_slab;   /* Precomputed: (4096 - sizeof(slab_t)) / obj_size */
} slab_cache_t;
```

9 `slab_cache_t` instances, statically allocated in `kheap.c`.

### Large Allocation Path (> 4096 bytes)

Requests > 4096 bytes go to PMM directly:

```
kmalloc(8192):
  → pages = (8192 + sizeof(large_hdr_t) + 4095) / 4096  // ceil
  → alloc pages via pmm_alloc_blocks(pages)
  → write LARGE_MAGIC + pages at the start
  → return (void*)((uint32_t)block + sizeof(large_hdr_t))
  
kfree(ptr):
  → header = (large_hdr_t*)((uint32_t)ptr - sizeof(large_hdr_t))
  → if header->magic == LARGE_MAGIC: pmm_free_blocks(header, header->pages)
```

```c
typedef struct {
    uint32_t magic;       /* LARGE_MAGIC */
    uint32_t pages;       /* Number of 4KB pages allocated */
} large_hdr_t;
```

### kmalloc/kfree Signatures (unchanged)

```c
void* kmalloc(size_t size);   // Returns NULL for size=0 or failure
void  kfree(void* ptr);        // No-op on NULL
```

The entire kernel continues to use the same API. No call site changes needed for basic functionality — the difference is that `kfree` now *actually frees memory*.

### New Heap API

```c
uint32_t kheap_get_used(void);       // Total slab+PMM memory committed
uint32_t kheap_get_free(void);       // Total free objects across all caches (bytes)
uint32_t kheap_get_current(void);    // Kept for compatibility: returns committed total
void     kheap_reclaim(void);        // Free completely-empty slabs back to PMM
```

### String Utility Functions

The string functions (memcpy, memset, strlen, strcmp, etc.) stay in `kheap.c`. They are not allocator code but they share the file and nothing else depends on them. **Do not move or touch them** — they work and changing their location breaks the build.

---

## 2. Call Site Audit & Fixes Enabled by Slab

### Sites that automatically benefit (no code change)

| File | Site | What changes |
|------|------|-------------|
| `tcc.c` | All kfree calls | `kfree(tokens)`, `kfree(s->output_buffer)`, `kfree(s)` now reclaim memory |
| `shell.c:902` | `kfree(source)` in mkcode | Actually frees the 4096-byte source buffer |
| `shell.c:931` | `kfree(source)` in cat/tcc | Actually frees after compilation |
| `exec.c:167,202,214,225,231` | `kfree(source)` and `kfree(file_data)` | ELF loader now reclaims per-file memory |
| `kernel.c:154` | `kmalloc(KERNEL_STACK_SIZE)` | 8KB → PMM-backed large alloc, works unchanged |

### Sites that need explicit fixes

**`editor.c:335`** — `edit_buffer = kmalloc(MAX_FILE_SIZE)` (8192 bytes). Currently never freed. **Fix:** Add `kfree(edit_buffer)` when the editor exits (user presses Ctrl+Q). Currently the editor has no exit path that frees the buffer.

**`task.c:110-111`** — Allocates kernel stack for new task via `kmalloc(12288)`. 12288 bytes rounds up to 3 PMM pages. **At task_exit:** must call `kfree(stack_ptr)` to reclaim the stack. Currently not done.

**`task.c:38,99`** — Allocates `task_t` (88 bytes → class 128). **At task_exit:** must call `kfree(task_ptr)`. Currently not done — tasks become zombies but memory is never freed.

**`shell.c:1174`** — `heapcheck` command calls `kheap_get_used()`. Must also call `kheap_get_free()` to show both committed and free memory.

**`net.c:25`** — `reply_buf` DMA race. This is a pre-existing bug that becomes fixable because we can now allocate + free dynamically. **Fix approach:** Instead of a static `reply_buf`, allocate a fresh buffer per ARP reply via `kmalloc(256)` and free it after the TX descriptor completes (on next TOK ISR or after a spin-wait). This eliminates the race because each reply has its own buffer that isn't touched by the ISR until it's freed.

---

## 3. Signal Handling

### Signals Defined

| Number | Name | Default Action | Can Catch? | Can Ignore? |
|--------|------|---------------|-----------|-------------|
| 1 | SIGHUP | terminate | yes | yes |
| 2 | SIGINT | terminate | yes | yes |
| 9 | SIGKILL | terminate | **no** | **no** |
| 15 | SIGTERM | terminate | yes | yes |
| 17 | SIGCHLD | ignore | yes | yes |

### task_t Additions

```c
// Added to the existing task_t struct:
uint32_t signal_pending;         /* Bitmask: pending signals (set by kill, cleared by delivery) */
uint32_t signal_blocked;         /* Bitmask: blocked signals (sigprocmask) */
void*    signal_handlers[32];    /* Per-signal handlers (NULL = SIG_DFL) */
```

Total added per task: 136 bytes (two uint32_t + 32 pointers). Current task_t is ~88 bytes, adding 136 → ~224 bytes. task_t moves from class 128 (class 3) to class 256 (class 4).

### Syscalls

```c
void* sys_signal(int sig, void* handler);
// sig: signal number (1-31)
// handler: SIG_IGN (1), SIG_DFL (0), or a function pointer
// Returns: previous handler, or SIG_ERR on invalid sig

int sys_kill(int pid, int sig);
// pid: target process ID
// sig: signal number
// Returns: 0 on success, -1 if pid not found or sig invalid
```

### Delivery Mechanism

Signal delivery happens in `task_switch()`, right after the scheduler picks the next task:

```
task_switch():
  1. Save current task context
  2. Pick next task from ready queue
  3. If next->signal_pending & ~next->signal_blocked:
     a. Pick lowest pending signal (ffs-style)
     b. Clear bit from signal_pending
     c. Look up action: next->signal_handlers[sig]
     d. If SIGKILL or (NULL && default_is_term(sig)):
        → next->state = STATE_ZOMBIE
        → Don't schedule this task — pick another
     e. If NULL && default_is_ignore(sig) (SIGCHLD):
        → Skip, continue scheduling
     f. If handler is SIG_IGN:
        → Skip, continue scheduling
     g. If handler is user-space function:
        → Push "signal trampoline" frame on the user stack
        → Set EIP to the handler function
        → When handler returns to the trampoline:
          → Trampoline calls sys_sigreturn (restores saved context)
  4. Switch to next task
```

### Integration with kill command

```
kill <pid>         → sys_kill(pid, SIGTERM)
kill -9 <pid>      → sys_kill(pid, SIGKILL)
kill -l            → list signal names and numbers
```

The existing `task_kill(pid)` function becomes a thin wrapper around `sys_kill(pid, SIGTERM)`.

---

## 4. Keyboard Buffering

### Ring Buffer

```c
#define KBD_BUF_SIZE 256

static volatile char kbd_buffer[KBD_BUF_SIZE];
static volatile uint16_t kbd_head = 0;   /* Written by ISR */
static volatile uint16_t kbd_tail = 0;   /* Written by shell */
```

ISR path:
```
keyboard_callback(regs):
  scancode = inb(0x60)
  translate to char c via kbdus/kbdus_shifted tables
  if c != 0:
    kbd_buffer[kbd_head % KBD_BUF_SIZE] = c
    kbd_head++
```

Shell path:
```
int keyboard_read(void):
  if kbd_head == kbd_tail: return -1              // empty
  c = kbd_buffer[kbd_tail % KBD_BUF_SIZE]
  kbd_tail++
  return c
```

### Shell Integration

The shell currently receives characters through a push callback (`shell_input(c)` called directly from the ISR). This changes to a pull model:

```c
// In shell_main() input loop:
while (1) {
    int c = keyboard_read();     // Non-blocking
    if (c >= 0) {
        shell_input(c);          // Process full character
    }
    // ... other shell work (or yield if multitasking) ...
}
```

For the keyboard to not miss characters during long kernel operations (compile, network), the shell must ensure it calls `keyboard_read()` frequently. This is already the case — the shell main loop has idle ticks where it could be polling. A `sleep(1)` or simple idle loop handles this.

### Ctrl+L (bonus)

Add `0x0C` (Ctrl+L) handling in `keyboard_callback`: sets a `request_clear_screen` flag that the shell acts on at the next `shell_input()` call.

---

## 5. Implementation Order

Sub-projects must be built in this order. Dependencies flow downward.

1. **Slab allocator** — `src/mm/kheap.c`, `src/include/kheap.h`
   - Implements: `kmalloc`, `kfree`, `init_kheap`, all 9 caches
   - No call site changes needed
   - Test: build, boot, run `heapcheck` before/after

2. **Call site fixes** — `editor.c`, `task.c`, `shell.c`, `net.c`
   - Wire up `kfree` for editor buffer, task structures, ARP reply buffers
   - Fix the `reply_buf` DMA race

3. **Signal handling** — `src/include/task.h`, `src/kernel/task.c`, `src/bin/shell.c`
   - Add signal fields to `task_t`
   - Add `sys_signal` and `sys_kill` syscalls
   - Wire delivery into `task_switch()`
   - Update `kill` shell command

4. **Keyboard buffering** — `src/drivers/keyboard.c`, `src/include/keyboard.h`, `src/bin/shell.c`
   - Add ring buffer, `keyboard_read()` function
   - Change shell input from push to pull
   - Add Ctrl+L support

---

## 6. Key Gotchas

1. **ISR must not call kmalloc.** The keyboard ISR writes to a static ring buffer — no allocation. Any future ISR that needs memory must use a pre-allocated pool or defer allocation via workqueue.

2. **Slab header is at the page start.** If someone `memset(ptr, 0, size)` on an allocated slab object, the data portion is zeroed but the slab header at the page start is not affected (the returned pointer is after the header). This is correct behavior.

3. **kfree(page-aligned pointer to a slab header) is wrong.** If someone accidentally frees the slab header address, `kfree` will interpret it as a massive object with a corrupt magic number and silently skip it. This is intentional — better silent than exploitable.

4. **The string functions in kheap.c are not allocator code.** They share the file for historical reasons. Do not move them — the build system expects them there.

5. **`kheap_get_used()` semantics change.** Old: "bytes consumed from bump pointer." New: "total committed memory across all caches + PMM large allocs." The number will be higher for the same workload because slabs pre-allocate full pages. This is expected.

6. **`kheap_get_current()` kept for compatibility** but the concept of "current pointer" no longer exists. It now returns `kheap_get_used()`.
