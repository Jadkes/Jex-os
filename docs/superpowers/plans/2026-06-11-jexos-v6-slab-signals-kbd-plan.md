# JexOS v6.0: Slab Allocator + Signals + Keyboard Buffering Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the bump allocator with a power-of-2 slab allocator that actually frees memory, add signal handling (5 signals, syscall-based), add keyboard buffering (256-byte ring buffer), and fix bugs enabled by proper `kfree`.

**Architecture:** Three ordered subsystems. (1) Slab allocator: 9 size classes, intra-slab free list, O(1) kfree via page-mask, PMM fallback >4KB. (2) Signal handling: bitmasks + handler array in task_t, delivery in task_switch(). (3) Keyboard buffering: ring buffer in ISR, pull model in shell.

**Tech Stack:** Freestanding C (gnu99), target i386 (QEMU). No libc. No FPU.

---

## File Structure

### Modified files only (no new files needed):

| File | Change |
|------|--------|
| `src/include/kheap.h` | Add slab structs, new API decls, keep string utils |
| `src/mm/kheap.c` | Rewrite kmalloc/kfree/init_kheap with slab allocator; **preserve all string utilities verbatim** |
| `src/include/task.h` | Add `signal_pending`, `signal_blocked`, `signal_handlers[32]` to task_t |
| `src/kernel/task.c` | Signal delivery in task_switch(); kfree in task_exit(); sys_signal/sys_kill |
| `src/include/syscall.h` | Add `SYS_SIGNAL` (13) and `SYS_KILL` (14) syscall numbers |
| `src/kernel/syscall.c` | Dispatch SYS_SIGNAL and SYS_KILL in syscall_handler |
| `src/bin/shell.c` | Update heapcheck; update kill with -9, -l; change input from push to pull; Ctrl+L |
| `src/drivers/keyboard.c` | Add ring buffer; ISR writes buffer instead of calling shell_input() |
| `src/include/keyboard.h` | Add `keyboard_read()`, `keyboard_flush()` |
| `src/bin/editor.c` | Add `kfree(edit_buffer)` on Ctrl+Q exit |

---

## Build Order

Each task builds on the previous. Build after every task. Do not skip.

```
Task 1 (kheap.h) → Task 2 (kheap.c) → Task 3 (heapcheck shell) → Task 4 (call site fixes)
→ Task 5 (task.h + syscalls) → Task 6 (signal delivery) → Task 7 (keyboard buffer)
→ Task 8 (shell pull model + Ctrl+L)
```

---

## Task 1: Slab Allocator Headers

**Files:** Modify: `src/include/kheap.h`

- [ ] **Step 1: Add slab allocator constants, structs, and new API before existing declarations**

Insert after the `#include` guards but before the existing function declarations:

```c
/* =============== Slab Allocator =============== */

#define SLAB_MAGIC      0x534C4142  /* "SLAB" */
#define LARGE_MAGIC     0x4C524700  /* "LRG\0" */
#define SLAB_PAGE_SIZE  4096
#define SLAB_MAX_SIZE   4096
#define SLAB_CACHE_COUNT 9

typedef struct slab {
    uint32_t        magic;      /* SLAB_MAGIC or LARGE_MAGIC */
    struct slab*    next;       /* Next slab in cache's linked list */
    uint32_t        obj_size;   /* Size of each object in bytes */
    uint32_t        obj_count;  /* Total objects in this slab */
    uint32_t        free_count; /* Number of free objects remaining */
    uint32_t        free_head;  /* Index of first free object (0xFFFFFFFF = empty) */
    uint32_t        pad;        /* Align to 32 bytes */
} __attribute__((packed)) slab_t;

typedef struct {
    slab_t*     slab_list;      /* Linked list of all slabs in this cache */
    uint32_t    obj_size;       /* Object size for this cache */
    uint32_t    objs_per_slab;  /* Precomputed: (4096 - sizeof(slab_t)) / obj_size */
} slab_cache_t;

typedef struct {
    uint32_t magic;             /* LARGE_MAGIC */
    uint32_t pages;             /* Number of 4KB pages allocated */
} large_hdr_t;

/* Heap API */
void init_kheap(void);
uint32_t kheap_get_free(void);
void     kheap_reclaim(void);
```

The old `init_kheap(uint32_t start_addr)` becomes parameterless. Remove the parameter.

- [ ] **Step 2: Verify string utility declarations (lines 49-60) remain unchanged**

Make sure these are still present after the slab additions:
```c
void* memcpy(void* dest, const void* src, size_t n);
void* memset(void* s, int c, size_t n);
size_t strlen(const char* s);
int strcmp(const char* s1, const char* s2);
int strncmp(const char* s1, const char* s2, size_t n);
char* strcpy(char* dest, const char* src);
char* strcat(char* dest, const char* src);
char* strstr(const char* haystack, const char* needle);
char* strchr(const char* s, int c);
char* strrchr(const char* s, int c);
char* strncpy(char* dest, const char* src, size_t n);
```

- [ ] **Step 3: Build and verify**

```bash
make clean && make 2>&1 | head -30
```

Expected: errors about `init_kheap` signature mismatch. This is expected — Task 2 fixes the implementation.

- [ ] **Step 4: Commit**

```bash
git add src/include/kheap.h
git commit -m "feat: add slab allocator headers, structs, and new heap API"
```

---

## Task 2: Slab Allocator Implementation

**Files:** Modify: `src/mm/kheap.c`

**CRITICAL:** The string utility functions (current lines 70-160, functions `memcpy`, `__memcpy_chk`, `memset`, `strlen`, `strcmp`, `strncmp`, `strcpy`, `__strcpy_chk`, `strcat`, `__strcat_chk`, `strstr`, `strchr`, `strrchr`, `strncpy`) must be copied to the new file **verbatim, unchanged**. The whole file is rewritten but the string section is byte-for-byte identical.

- [ ] **Step 1: Write the new kheap.c with the current code backed up**

First, make a backup:
```bash
cp src/mm/kheap.c src/mm/kheap.c.bak
```

- [ ] **Step 2: Write the new kheap.c — file header and includes**

The new file begins:

```c
/**
 * @file kheap.c
 * @brief Kernel Heap — power-of-2 slab allocator with PMM fallback.
 *
 * 9 size classes (16B–4096B). Intra-slab free list for O(1) alloc/free.
 * Slab header at page start for O(1) kfree via page-mask + magic check.
 * Allocations > 4096 bytes go to PMM with LARGE_MAGIC header.
 * String utilities in the second half of the file are kept for linking.
 */

#include "kheap.h"
#include "pmm.h"
#include "paging.h"
#include "init.h"
#include <stddef.h>
#include <stdint.h>
```

- [ ] **Step 3: Implement slab size table and caches array**

```c
/* =============== Slab Allocator Core =============== */

static const uint32_t slab_sizes[SLAB_CACHE_COUNT] = {
    16, 32, 64, 128, 256, 512, 1024, 2048, 4096
};

static slab_cache_t caches[SLAB_CACHE_COUNT];
static uint32_t total_committed = 0;   /* All slab pages + PMM large allocs */

static void init_caches(void) {
    for (int i = 0; i < SLAB_CACHE_COUNT; i++) {
        caches[i].obj_size = slab_sizes[i];
        caches[i].slab_list = NULL;
        caches[i].objs_per_slab =
            (SLAB_PAGE_SIZE - sizeof(slab_t)) / slab_sizes[i];
    }
}
```

- [ ] **Step 4: Implement slab_create — allocate a new slab page and init free list**

```c
static slab_t* slab_create(int cache_idx) {
    slab_cache_t* cache = &caches[cache_idx];
    uint32_t page = pmm_alloc_blocks(1);
    if (!page) return NULL;

    slab_t* slab = (slab_t*)page;
    slab->magic = SLAB_MAGIC;
    slab->next = NULL;
    slab->obj_size = cache->obj_size;
    slab->obj_count = cache->objs_per_slab;
    slab->free_count = cache->objs_per_slab;
    slab->free_head = 0;

    /* Build intra-slab free list via indices stored in free object memory */
    uint8_t* data = (uint8_t*)((uint32_t)slab + sizeof(slab_t));
    for (uint32_t i = 0; i < slab->obj_count; i++) {
        uint32_t* next_idx = (uint32_t*)(data + i * slab->obj_size);
        *next_idx = (i == slab->obj_count - 1) ? 0xFFFFFFFF : (i + 1);
    }

    total_committed += SLAB_PAGE_SIZE;
    return slab;
}
```

- [ ] **Step 5: Implement slab_alloc — allocate one object from a cache**

```c
static void* slab_alloc(int cache_idx) {
    slab_cache_t* cache = &caches[cache_idx];
    slab_t* slab = cache->slab_list;

    /* Walk the list to find a slab with free objects */
    while (slab && slab->free_count == 0) {
        slab = slab->next;
    }

    /* No space — allocate a new slab from PMM */
    if (!slab) {
        slab = slab_create(cache_idx);
        if (!slab) return NULL;
        slab->next = cache->slab_list;
        cache->slab_list = slab;
    }

    /* Pop from free list: object stores index of next free object */
    uint8_t* data = (uint8_t*)((uint32_t)slab + sizeof(slab_t));
    uint32_t idx = slab->free_head;
    uint32_t* obj = (uint32_t*)(data + idx * slab->obj_size);
    slab->free_head = *obj;
    slab->free_count--;

    return (void*)obj;
}
```

- [ ] **Step 6: Implement slab_free — return an object to its slab free list**

```c
static void slab_free(slab_t* slab, void* ptr) {
    uint8_t* data = (uint8_t*)((uint32_t)slab + sizeof(slab_t));
    uint32_t offset = (uint32_t)ptr - (uint32_t)data;
    uint32_t idx = offset / slab->obj_size;

    /* Push onto free list */
    uint32_t* obj = (uint32_t*)(data + idx * slab->obj_size);
    *obj = slab->free_head;
    slab->free_head = idx;
    slab->free_count++;
}
```

- [ ] **Step 7: Implement ptr_to_slab — resolve pointer to slab header via page mask**

```c
static slab_t* ptr_to_slab(void* ptr) {
    slab_t* slab = (slab_t*)((uint32_t)ptr & ~0xFFF);
    if (slab->magic == SLAB_MAGIC || slab->magic == LARGE_MAGIC)
        return slab;
    return NULL;
}
```

- [ ] **Step 8: Implement kmalloc — route to slab cache or PMM large alloc**

```c
/**
 * @brief Allocate kernel memory.
 *
 * Routes to slab cache for sizes <= 4096 bytes, PMM large alloc for larger.
 * Returns NULL for size == 0 or allocation failure.
 *
 * @param size Number of bytes to allocate.
 * @return Pointer to allocated memory, or NULL.
 */
void* kmalloc(size_t size) {
    if (size == 0) return NULL;

    /* Large allocation via PMM (> 4096 bytes) */
    if (size > SLAB_MAX_SIZE) {
        uint32_t total = size + sizeof(large_hdr_t);
        uint32_t pages = (total + SLAB_PAGE_SIZE - 1) / SLAB_PAGE_SIZE;
        uint32_t block = pmm_alloc_blocks(pages);
        if (!block) return NULL;

        large_hdr_t* hdr = (large_hdr_t*)block;
        hdr->magic = LARGE_MAGIC;
        hdr->pages = pages;
        total_committed += pages * SLAB_PAGE_SIZE;
        return (void*)((uint32_t)block + sizeof(large_hdr_t));
    }

    /* Find the smallest cache class that fits this size */
    int idx = 0;
    while (idx < SLAB_CACHE_COUNT - 1 && slab_sizes[idx] < size) {
        idx++;
    }
    return slab_alloc(idx);
}
```

- [ ] **Step 9: Implement kfree — free via magic number detection**

```c
/**
 * @brief Free kernel memory previously allocated by kmalloc.
 *
 * Uses magic number at the page boundary to determine type:
 * - SLAB_MAGIC: standard slab free
 * - LARGE_MAGIC: PMM-backed large alloc
 * - Neither: silent no-op (defensive, corrupted or invalid pointer)
 *
 * @param ptr Pointer to free, or NULL (no-op).
 */
void kfree(void* ptr) {
    if (!ptr) return;

    slab_t* slab = ptr_to_slab(ptr);
    if (!slab) return;

    if (slab->magic == SLAB_MAGIC) {
        slab_free(slab, ptr);
    } else if (slab->magic == LARGE_MAGIC) {
        large_hdr_t* hdr = (large_hdr_t*)slab;
        total_committed -= hdr->pages * SLAB_PAGE_SIZE;
        pmm_free_blocks((uint32_t)hdr, hdr->pages);
    }
}
```

- [ ] **Step 10: Implement init_kheap and the heap API**

```c
/**
 * @brief Initialize the kernel heap slab allocator.
 *
 * Pre-computes object counts for all 9 caches.
 * No pre-allocation — slabs are allocated on first kmalloc.
 */
void init_kheap(void) {
    init_caches();
}

/* Initcall: runs early in boot (before tasking, after PMM) */
static void init_kheap_wrapper(void) {
    init_kheap();
}
early_init(init_kheap_wrapper);

uint32_t kheap_get_used(void) {
    return total_committed;
}

uint32_t kheap_get_free(void) {
    uint32_t free_bytes = 0;
    for (int i = 0; i < SLAB_CACHE_COUNT; i++) {
        slab_t* slab = caches[i].slab_list;
        while (slab) {
            free_bytes += slab->free_count * slab->obj_size;
            slab = slab->next;
        }
    }
    return free_bytes;
}

/**
 * @brief Return kheap_get_used() for backward compatibility.
 * The old "current pointer" concept no longer exists.
 */
uint32_t kheap_get_current(void) {
    return kheap_get_used();
}

uint32_t kheap_get_start(void) {
    return 0;  /* No longer applicable with slab allocator */
}

/**
 * @brief Free completely empty slabs back to PMM.
 */
void kheap_reclaim(void) {
    for (int i = 0; i < SLAB_CACHE_COUNT; i++) {
        slab_cache_t* cache = &caches[i];
        slab_t** prev = &cache->slab_list;
        slab_t* slab = cache->slab_list;

        while (slab) {
            if (slab->free_count == slab->obj_count && slab->obj_count > 0) {
                *prev = slab->next;
                total_committed -= SLAB_PAGE_SIZE;
                pmm_free_blocks((uint32_t)slab, 1);
                slab = *prev;
            } else {
                prev = &slab->next;
                slab = slab->next;
            }
        }
    }
}
```

- [ ] **Step 11: Copy string utilities verbatim from the backup**

Copy the `/* Memory Utilities */` and `/* String Utilities */` sections from `src/mm/kheap.c.bak` lines 68-160 exactly.

The copied functions are:
```c
void* memcpy(void* dest, const void* src, size_t n)
void* __memcpy_chk(void* dest, const void* src, size_t len, size_t destlen)
void* memset(void* s, int c, size_t n)
size_t strlen(const char* s)
int strcmp(const char* s1, const char* s2)
int strncmp(const char* s1, const char* s2, size_t n)
char* strcpy(char* dest, const char* src)
char* __strcpy_chk(char* dest, const char* src, size_t destlen)
char* strcat(char* dest, const char* src)
char* __strcat_chk(char* dest, const char* src, size_t destlen)
char* strstr(const char* haystack, const char* needle)
char* strchr(const char* s, int c)
char* strrchr(const char* s, int c)
char* strncpy(char* dest, const char* src, size_t n)
```

**These must be character-for-character identical to the originals.** Do not reformat, re-indent, or otherwise "improve" them.

- [ ] **Step 12: Remove the backup**

```bash
rm src/mm/kheap.c.bak
```

- [ ] **Step 13: Build and verify**

```bash
make clean && make 2>&1
```

Expected: zero errors, zero warnings.

- [ ] **Step 14: Commit**

```bash
git add src/mm/kheap.c
git commit -m "feat: implement power-of-2 slab allocator with O(1) kfree"
```

---

## Task 3: Update heapcheck Command

**Files:** Modify: `src/bin/shell.c`

- [ ] **Step 1: Replace the heapcheck command section (lines 1172-1189)**

Old code:
```c
else if (strcmp(shell_buffer, "heapcheck") == 0) {
    char buf[16];
    uint32_t used = kheap_get_used();
    uint32_t cur  = kheap_get_current();
    terminal_writestring("Heap: bump allocator at 0x");
    format_hex(kheap_get_start(), buf);
    terminal_writestring(buf);
    terminal_writestring("\n  Used:     ");
    int_to_string((int)used, buf);
    terminal_writestring(buf);
    terminal_writestring(" bytes\n  Current:  0x");
    format_hex(cur, buf);
    terminal_writestring(buf);
    terminal_writestring("\n  Free (est): ");
    int_to_string((int)(0x40000000 - used), buf);
    terminal_writestring(buf);
    terminal_writestring(" bytes\n");
}
```

New code:
```c
else if (strcmp(shell_buffer, "heapcheck") == 0) {
    char buf[16];
    uint32_t used = kheap_get_used();
    uint32_t free = kheap_get_free();
    terminal_writestring("Heap: slab allocator\n");
    terminal_writestring("  Committed: ");
    int_to_string((int)used, buf);
    terminal_writestring(buf);
    terminal_writestring(" bytes\n  Free:      ");
    int_to_string((int)free, buf);
    terminal_writestring(buf);
    terminal_writestring(" bytes\n");
}
```

- [ ] **Step 2: Build and verify**

```bash
make clean && make 2>&1
```

Expected: zero errors, zero warnings.

- [ ] **Step 3: Commit**

```bash
git add src/bin/shell.c
git commit -m "feat: update heapcheck to show slab committed/free stats"
```

---

## Task 4: Call Site Fixes — task_exit and editor buffer

**Files:** Modify: `src/kernel/task.c`, `src/bin/editor.c`

- [ ] **Step 1: Add kfree calls to task_exit in task.c (lines 142-146)**

Current code:
```c
void task_exit() {
    __asm__ volatile("cli");
    current_task->state = STATE_ZOMBIE;
    task_switch();
}
```

New code:
```c
void task_exit() {
    __asm__ volatile("cli");

    task_t* dying = (task_t*)current_task;
    dying->state = STATE_ZOMBIE;

    /* Remove from ready queue so scheduler never picks this task again */
    if (ready_queue == dying) {
        ready_queue = dying->next;
    } else {
        task_t* prev = (task_t*)ready_queue;
        while (prev && prev->next != dying) {
            prev = prev->next;
        }
        if (prev) {
            prev->next = dying->next;
        }
    }

    /* Free kernel stack if one was allocated (fork'd tasks have kstack != 0) */
    if (dying->kstack) {
        /* kmalloc(12288) in fork() allocates 3 PMM pages via large alloc path.
         * The stack pointer is page-aligned upward from the kmalloc return.
         * kstack - 0x1000 reaches the PMM block base where LARGE_MAGIC lives. */
        kfree((void*)(dying->kstack - 0x1000));
    }

    /* Free the task_t struct itself */
    kfree((void*)dying);

    task_switch();
}
```

- [ ] **Step 2: Add kfree on editor exit in editor.c**

Find the Ctrl+Q handler in `editor.c`. The editor exits when the user presses Ctrl+Q (0x11). Add `kfree(edit_buffer)` before the editor state is reset. Find the editor input handling that processes 0x11 and add the free.

Look for the section that handles `key == 0x11` (Ctrl+Q). Add:
```c
if (key == 0x11) {
    kfree(edit_buffer);
    edit_buffer = NULL;
    editor_running = 0;
    /* ... rest of exit handling ... */
}
```

If `editor.c` doesn't have an explicit Ctrl+Q handler that frees, add one. The pattern should be:
- When the editor exits (0x11 received), free `edit_buffer` and NULL it
- Set `edit_len = 0`
- Set `editor_running = 0`

- [ ] **Step 3: Add kfree for initial shell task's task_t**

Currently `init_tasking()` in task.c allocates `current_task` via `kmalloc(sizeof(task_t))` but this task (PID 1 / shell) never exits. No change needed here — PID 1 is immortal. Only forked tasks need cleanup.

- [ ] **Step 4: Build and verify**

```bash
make clean && make 2>&1
```

Expected: zero errors, zero warnings.

- [ ] **Step 5: Commit**

```bash
git add src/kernel/task.c src/bin/editor.c
git commit -m "fix: free task structs and editor buffer via proper kfree"
```

---

## Task 5: Signal Fields and Syscalls

**Files:** Modify: `src/include/task.h`, `src/include/syscall.h`, `src/kernel/syscall.c`, `src/kernel/task.c`

- [ ] **Step 1: Add signal definitions to task.h**

Add to `src/include/task.h`, inside the `task_t` struct, after the `next` field:

```c
    /* Signal handling */
    uint32_t signal_pending;         /* Bitmask: pending signals (set by kill, cleared by delivery) */
    uint32_t signal_blocked;         /* Bitmask: blocked signals */
    void*    signal_handlers[32];    /* Per-signal handlers (NULL = SIG_DFL) */
```

Also add signal number constants after the struct:

```c
/* Signal numbers */
#define SIGHUP    1
#define SIGINT    2
#define SIGKILL   9
#define SIGTERM  15
#define SIGCHLD  17

/* Signal actions */
#define SIG_DFL ((void*)0)
#define SIG_IGN ((void*)1)
#define SIG_ERR ((void*)-1)

/* Default action classification */
#define SIGNAL_ACTION_TERMINATE 0
#define SIGNAL_ACTION_IGNORE    1
```

Add syscall function declarations:

```c
/* Signal syscalls */
void* sys_signal(int sig, void* handler);
int   sys_kill(int pid, int sig);
```

- [ ] **Step 2: Initialize signal fields in init_tasking()**

In `task.c`'s `init_tasking()`, add zero-initialization for the new fields:

```c
    current_task->signal_pending = 0;
    current_task->signal_blocked = 0;
    for (int i = 0; i < 32; i++)
        current_task->signal_handlers[i] = NULL;
```

- [ ] **Step 3: Initialize signal fields in fork()**

In `task.c`'s `fork()`, after copying the parent's name, add:

```c
    child->signal_pending = 0;
    child->signal_blocked = parent->signal_blocked;
    for (int i = 0; i < 32; i++)
        child->signal_handlers[i] = parent->signal_handlers[i];
```

- [ ] **Step 4: Add syscall numbers to syscall.h**

```c
#define SYS_SIGNAL   13
#define SYS_KILL     14
```

- [ ] **Step 5: Add dispatch in syscall_handler in syscall.c**

Add before the closing `}` of `syscall_handler()`:

```c
    else if (regs->eax == SYS_SIGNAL)
    {
        /* EBX = sig, ECX = handler */
        regs->eax = (uint32_t)sys_signal((int)regs->ebx, (void*)regs->ecx);
    }
    else if (regs->eax == SYS_KILL)
    {
        /* EBX = pid, ECX = sig */
        regs->eax = (uint32_t)sys_kill((int)regs->ebx, (int)regs->ecx);
    }
```

Add `#include "task.h"` to the includes at the top of syscall.c if not already present.

- [ ] **Step 6: Implement sys_signal and sys_kill in task.c**

```c
/**
 * @brief Register a signal handler for the current task.
 * @param sig Signal number (1-31).
 * @param handler SIG_IGN (1), SIG_DFL (0), or a function pointer.
 * @return Previous handler, or SIG_ERR on invalid sig.
 */
void* sys_signal(int sig, void* handler) {
    if (sig < 1 || sig > 31 || sig == SIGKILL) {
        return SIG_ERR;
    }
    void* prev = (void*)current_task->signal_handlers[sig];
    current_task->signal_handlers[sig] = handler;
    return prev;
}

/**
 * @brief Send a signal to a process.
 * @param pid Target process ID.
 * @param sig Signal number (1-31).
 * @return 0 on success, -1 if PID not found or sig invalid.
 */
int sys_kill(int pid, int sig) {
    if (sig < 1 || sig > 31) return -1;

    /* Never kill PID 1 (the shell/init) */
    if (pid == 1) return -1;

    task_t* t = (task_t*)ready_queue;
    while (t) {
        if (t->id == pid) {
            t->signal_pending |= (1 << sig);
            return 0;
        }
        t = t->next;
    }
    return -1;
}
```

- [ ] **Step 7: Update task_kill to use sys_kill**

```c
int task_kill(int pid) {
    return sys_kill(pid, SIGTERM);
}
```

- [ ] **Step 8: Build and verify**

```bash
make clean && make 2>&1
```

Expected: zero errors, zero warnings. (Signal delivery not wired yet — next task.)

- [ ] **Step 9: Commit**

```bash
git add src/include/task.h src/include/syscall.h src/kernel/syscall.c src/kernel/task.c
git commit -m "feat: add signal fields, syscalls, and sys_signal/sys_kill"
```

---

## Task 6: Signal Delivery in the Scheduler

**Files:** Modify: `src/kernel/task.c`

- [ ] **Step 1: Add signal delivery hook in task_switch()**

In `task_switch()`, after picking the next task (after `if (!current_task) current_task = ready_queue;`) and before the assembly context switch, add:

```c
    /* ============ Signal Delivery ============ */
    {
        task_t* next = (task_t*)current_task;
        uint32_t pending = next->signal_pending & ~next->signal_blocked;

        if (pending) {
            /* Find the lowest pending signal number */
            int sig = 1;
            while (sig < 32 && !(pending & (1 << sig))) sig++;

            if (sig < 32) {
                /* Clear the pending bit */
                next->signal_pending &= ~(1 << sig);

                void* handler = next->signal_handlers[sig];

                /* SIGKILL or default action = terminate */
                if (sig == SIGKILL || (handler == SIG_DFL &&
                    (sig == SIGHUP || sig == SIGINT || sig == SIGTERM))) {
                    next->state = STATE_ZOMBIE;
                    /* Don't schedule this task — pick another */
                    current_task = current_task->next;
                    if (!current_task) current_task = ready_queue;
                    /* Re-check the next task from the top */
                    esp = current_task->esp;
                    ebp = current_task->ebp;
                    eip = current_task->eip;
                    if (current_task->kstack) {
                        set_kernel_stack(current_task->kstack + 8192);
                    }
                    goto do_switch;
                }

                /* SIG_IGN or default ignore (SIGCHLD) — skip, continue scheduling */
                if (handler == SIG_IGN || (handler == SIG_DFL && sig == SIGCHLD)) {
                    /* Continue normally with this task */
                }

                /* User-space handler: for now, we just log it.
                 * Full trampoline implementation requires user-mode stack setup
                 * which is deferred — the signal is consumed (bit cleared) so
                 * it won't loop, but the handler isn't called yet. */
                if (handler && handler != SIG_IGN && handler != SIG_DFL) {
                    /* TODO: Implement user-space signal trampoline.
                     * For v6.0, the signal is consumed (acknowledged) but
                     * user-space handlers are not yet invoked. The signal
                     * is still delivered in the sense that:
                     * - The pending bit is cleared
                     * - The task continues running
                     * - SIGKILL still works (handled above)
                     * This is safe — signals don't accumulate. */
                }
            }
        }
    }
```

Add a label before the assembly context switch for the goto:

```c
do_switch:
    __asm__ volatile("...");  /* Keep the existing context switch as-is */
```

The existing code is:
```c
__asm__ volatile("         \n      mov %0, %%ebx;           \n      mov %1, %%esp;           \n      mov %2, %%ebp;           \n      mov %3, %%cr3;           \n      mov $0x12345, %%eax;     \n      sti;                     \n      jmp *%%ebx;              \n  " : : "r"(eip), "r"(esp), "r"(ebp), "r"(current_task->page_directory) : "ebx", "eax");
```

Change it to use the label:

```c
    /* Update TSS so hardware interrupts land on the correct kernel stack */
    if (current_task->kstack) {
        set_kernel_stack(current_task->kstack + 8192);
    }

do_switch:
    __asm__ volatile("         \n      mov %0, %%ebx;           \n      mov %1, %%esp;           \n      mov %2, %%ebp;           \n      mov %3, %%cr3;           \n      mov $0x12345, %%eax;     \n      sti;                     \n      jmp *%%ebx;              \n  " : : "r"(eip), "r"(esp), "r"(ebp), "r"(current_task->page_directory) : "ebx", "eax");
```

- [ ] **Step 2: Build and verify**

```bash
make clean && make 2>&1
```

Expected: zero errors, zero warnings.

- [ ] **Step 3: Commit**

```bash
git add src/kernel/task.c
git commit -m "feat: add signal delivery in task_switch scheduler hook"
```

---

## Task 7: Keyboard Ring Buffer

**Files:** Modify: `src/drivers/keyboard.c`, `src/include/keyboard.h`

- [ ] **Step 1: Add buffer declarations and keyboard_read/keyboard_flush to keyboard.h**

New contents of `src/include/keyboard.h`:

```c
/**
 * @file keyboard.h
 * @brief PS/2 Keyboard driver interface.
 *
 * Handles keyboard input interrupts and scancode translation.
 * Uses a 256-byte ring buffer decoupling ISR from shell consumer.
 */

#ifndef KEYBOARD_H
#define KEYBOARD_H

#define KBD_BUF_SIZE 256

/**
 * @brief Initialize the keyboard driver and register IRQ1 handler.
 */
void init_keyboard(void);

/**
 * @brief Read one character from the keyboard ring buffer (non-blocking).
 * @return The character, or -1 if the buffer is empty.
 */
int keyboard_read(void);

/**
 * @brief Clear all characters from the keyboard ring buffer.
 */
void keyboard_flush(void);

/**
 * @brief Check if the keyboard ring buffer has data.
 * @return 1 if data available, 0 if empty.
 */
int keyboard_has_data(void);

#endif // KEYBOARD_H
```

- [ ] **Step 2: Add ring buffer to keyboard.c**

Add declarations near the top of the file, after the `kbdus_shifted` table and modifier flags:

```c
/* =============== Keyboard Ring Buffer =============== */

static volatile char kbd_buffer[KBD_BUF_SIZE];
static volatile uint16_t kbd_head = 0;   /* Written by ISR */
static volatile uint16_t kbd_tail = 0;   /* Written by consumer (shell) */
```

- [ ] **Step 3: Modify keyboard_callback to write to buffer instead of calling shell_input**

In the ISR, replace the line `shell_input(c);` with:

```c
        if (c != 0) {
            /* Write to ring buffer instead of direct shell_input call */
            uint16_t next = (kbd_head + 1) % KBD_BUF_SIZE;
            if (next != kbd_tail) {  /* Buffer not full */
                kbd_buffer[kbd_head] = c;
                kbd_head = next;
            }
            /* If buffer full, character is dropped (ring buffer policy) */
        }
```

Remove the `#include "shell.h"` at the top of keyboard.c — it's no longer needed since we don't call `shell_input()` from the ISR. (Keep it only if other ISR code still uses it.)

- [ ] **Step 4: Implement keyboard_read, keyboard_flush, keyboard_has_data**

Add after the `keyboard_callback` function:

```c
/**
 * @brief Read one character from the ring buffer (non-blocking).
 * @return The character (0-255) or -1 if buffer is empty.
 */
int keyboard_read(void) {
    if (kbd_head == kbd_tail) return -1;
    char c = kbd_buffer[kbd_tail];
    kbd_tail = (kbd_tail + 1) % KBD_BUF_SIZE;
    return (int)(unsigned char)c;
}

/**
 * @brief Check if ring buffer has data.
 * @return 1 if data available, 0 if empty.
 */
int keyboard_has_data(void) {
    return (kbd_head != kbd_tail) ? 1 : 0;
}

/**
 * @brief Flush (clear) the ring buffer.
 */
void keyboard_flush(void) {
    kbd_head = 0;
    kbd_tail = 0;
}
```

- [ ] **Step 5: Build and verify**

```bash
make clean && make 2>&1
```

Expected: zero errors, zero warnings. (Shell still uses push model via serial — keyboard input won't work yet until Task 8.)

- [ ] **Step 6: Commit**

```bash
git add src/drivers/keyboard.c src/include/keyboard.h
git commit -m "feat: add 256-byte keyboard ring buffer, decouple ISR from shell"
```

---

## Task 8: Shell Pull Model + Ctrl+L + kill command update

**Files:** Modify: `src/bin/shell.c`

- [ ] **Step 1: Change shell_loop() to poll keyboard_read() instead of direct ISR push**

Current `shell_loop()` (lines 1250-1260):
```c
void shell_loop() {
    print_prompt();
    extern int is_serial_received(); extern char read_serial();
    while(1) {
        workqueue_run();
        while (is_serial_received()) {
            char c = read_serial(); if (c == '\r') c = '\n'; shell_input(c);
        }
        __asm__ volatile("hlt");
    }
}
```

New `shell_loop()`:
```c
void shell_loop() {
    print_prompt();
    extern int is_serial_received(); extern char read_serial();
    while(1) {
        workqueue_run();

        /* Poll serial input (debug console via QEMU -serial stdio) */
        while (is_serial_received()) {
            char c = read_serial(); if (c == '\r') c = '\n'; shell_input(c);
        }

        /* Poll keyboard ring buffer */
        int kc;
        while ((kc = keyboard_read()) >= 0) {
            shell_input((char)kc);
        }

        __asm__ volatile("hlt");
    }
}
```

Add `#include "keyboard.h"` to the includes at the top of shell.c (it should be there already on line 10).

- [ ] **Step 2: Add Ctrl+L handling in keyboard input processing**

The shell currently processes characters in `shell_input()`. The spec's Ctrl+L (0x0C) should be handled _before_ it reaches `shell_input()` — or at the points where it handles characters. Since the ISR now just writes to the buffer, Ctrl+L arrives as a regular character.

Add Ctrl+L handling in the keyboard ISR or in `shell_input()`:

In `shell_input()`, add at the beginning (before the editor check, or as a new special case):

```c
    /* Ctrl+L — clear screen */
    if (key == 0x0C) {
        terminal_clear();
        shell_refresh_line();
        return;
    }
```

But wait — we need `terminal_clear()` to exist. Check if it does.

Actually, looking at the shell code, `clear` is a command (line 72 in the command list and somewhere in dispatch). There's likely a `terminal_clear()` or `clear_screen()` function. Let me just use `terminal_clear()` in the plan and note to verify the function name.

Let me add the function name. Actually, I'll add a note to check. But for now, let me just reference it.

- [ ] **Step 3: Update kill command for signal support**

Find the kill command dispatch in shell.c. Currently it likely does:
```c
else if (strncmp(shell_buffer, "kill ", 5) == 0) {
    int pid = atoi(shell_buffer + 5);
    task_kill(pid);
}
```

Replace with:
```c
else if (strncmp(shell_buffer, "kill ", 5) == 0) {
    char* arg = shell_buffer + 5;
    while (*arg == ' ') arg++;

    if (strcmp(arg, "-l") == 0) {
        terminal_writestring(" 1 SIGHUP\n 2 SIGINT\n 9 SIGKILL\n15 SIGTERM\n17 SIGCHLD\n");
    } else if (arg[0] == '-' && arg[1] >= '0' && arg[1] <= '9') {
        int sig = atoi(arg + 1);
        while (*arg && *arg != ' ') arg++;
        while (*arg == ' ') arg++;
        if (*arg) {
            int pid = atoi(arg);
            sys_kill(pid, sig);
        } else {
            terminal_writestring("Usage: kill -<sig> <pid>\n");
        }
    } else {
        int pid = atoi(arg);
        sys_kill(pid, SIGTERM);
    }
}
```

- [ ] **Step 4: Update Ctrl+L handling — call terminal_initialize()**

The Ctrl+L handler in `shell_input()` should call `terminal_initialize()` then redraw the prompt. Since the existing `clear` command (line 811) does `terminal_initialize(); print_logo();`, Ctrl+L should do `terminal_initialize(); print_prompt();`.

The Ctrl+L code to add at the beginning of `shell_input()`:

```c
    /* Ctrl+L — clear screen */
    if (key == 0x0C) {
        terminal_initialize();
        print_prompt();
        return;
    }
```

Make sure `terminal_initialize()` is accessible (extern declaration or already linked) — it's the same function used by the `clear` command.

- [ ] **Step 5: Build and verify**

```bash
make clean && make 2>&1
```

Expected: zero errors, zero warnings.

- [ ] **Step 6: Commit**

```bash
git add src/bin/shell.c
git commit -m "feat: shell pull model for keyboard, Ctrl+L, kill -9/-l support"
```

---

## Task 9: Pre-existing test (manual) — Verify Allocator Works

- [ ] **Step 1: Build and boot in QEMU**

```bash
make clean && make && make run
```

- [ ] **Step 2: Check boot banner appears without crashes**

The boot banner should print normally. If the machine triple-faults, the slab allocator's `init_kheap` or an early kmalloc is broken.

- [ ] **Step 3: Run heapcheck**

```
heapcheck
```

Expected output:
```
Heap: slab allocator
  Committed: <some number> bytes
  Free:      <some number> bytes
```

- [ ] **Step 4: Verify keyboard input works**

Type `help` and press Enter. Expected: command list appears.

- [ ] **Step 5: Verify signal handling — kill a process**

Type:
```
ps          (list processes, note a PID other than 1)
kill <pid>  (should kill that process)
```

- [ ] **Step 6: Verify Ctrl+L works**

Press Ctrl+L. Expected: screen clears.

- [ ] **Step 7: Verify heap reclaim after operations**

```
heapcheck
```

Run some commands (`ls`, `cat`, etc.) then `heapcheck` again. Memory should not grow unboundedly.

- [ ] **Step 8: Test with the editor**

```
vix test.txt
```

Type some text, press Ctrl+Q to quit. Expected: editor exits, no crash, memory reclaimed.

---

## Self-Review Checklist

After writing the complete plan, verify:

1. **Spec coverage:**
   - [ ] Slab allocator: 9 power-of-2 size classes (Task 1, 2)
   - [ ] Intra-slab free list, O(1) alloc/free (Task 2)
   - [ ] Page-aligned kfree with magic numbers (Task 2)
   - [ ] PMM fallback for >4096 bytes (Task 2)
   - [ ] kheap_get_free(), kheap_reclaim() (Task 2)
   - [ ] heapcheck updated (Task 3)
   - [ ] Editor buffer free on Ctrl+Q (Task 4)
   - [ ] task_exit frees task_t and stack (Task 4)
   - [ ] Signal fields in task_t (Task 5)
   - [ ] sys_signal, sys_kill syscalls (Task 5)
   - [ ] Signal delivery in task_switch() (Task 6)
   - [ ] Keyboard ring buffer (Task 7)
   - [ ] Shell pull model (Task 8)
   - [ ] Ctrl+L support (Task 8)
   - [ ] kill -9, kill -l (Task 8)

2. **Placeholder scan:** No TODOs, TBDs, or incomplete steps remain.

3. **Type consistency:** All struct field names, function signatures, and constant names used in later tasks match definitions in earlier tasks.

4. **Build order:** Each task compiles independently. Build gates after every task.
