# JexOS Developer Experience Overhaul

> **For agentic workers:** This spec covers 7 independent phases of kernel improvements. Each phase produces working, testable code. Phases build on each other and should be implemented in order.

**Goal:** Make JexOS kernel development as pleasant as working on a mature kernel — with proper debugging tools, a driver model, runtime introspection, concurrency primitives, and a usable shell environment.

**Architecture:** Monolithic 32-bit x86 i386 kernel. All improvements are additive — no existing functionality is removed. Each phase is independently buildable and testable.

**Tech Stack:** gcc (freestanding with `-m32 -ffreestanding -std=gnu99`), ld with custom linker script, JexFS (native filesystem), serial (COM1), IDE/PIIX4, RTL8139.

---

## Phase 1: Dev Tools — WARN_ON, dump_stack, kallsyms, Stack Guards

### dump_stack — Reusable stack unwinder

Extract `unwind_stack()` from `panic.c` into its own compilation unit so both the panic handler and WARN_ON can share it.

**File:** `src/include/kernel/backtrace.h`
**File:** `src/kernel/backtrace.c`

```c
// include/kernel/backtrace.h
#ifndef BACKTRACE_H
#define BACKTRACE_H
#include <stdint.h>

int  unwind_stack(uint32_t ebp, uint32_t* eip_out, int max_frames);
void dump_stack(void);           // prints to terminal + serial
void dump_stack_serial(void);     // serial-only (safe in ISR)

#endif
```

- `unwind_stack()` — walks the frame pointer chain (same logic as current panic.c, including the EBP-alignment safety check).
- `dump_stack()` — calls unwind_stack, formats EIPs as hex via `format_hex()`, writes to both terminal and serial.
- `dump_stack_serial()` — same but serial-only, safe to call from ISR where the terminal may not be stable.

### WARN_ON — Non-fatal assertion

**File:** `src/include/kernel/warn.h`

```c
#define WARN_ON(condition) do {                                         \
    if ((condition)) {                                                   \
        log_serial("WARN_ON: " #condition " at %s:%d\n", __FILE__, __LINE__); \
        dump_stack_serial();                                             \
    }                                                                    \
} while(0)
```

Prints the condition text, file/line, and a stack trace, then continues execution. For any unexpected condition that isn't fatal (bad pointer in a statistics path, unexpected packet type, etc.).

### kallsyms — Symbolic backtraces

**File:** `src/include/kernel/kallsyms.h`
**File:** `src/kernel/kallsyms.c`
**New build step:** `tools/gen_kallsyms.c` — post-link symbol extractor
**Modified:** `linker.ld` — add `.kallsyms` section

**Data structure:**

```c
// Packed symbol entry — 8 bytes each, sorted by address at build time
typedef struct {
    uint32_t addr;        // Function start address
    uint32_t name_off : 24;  // Offset into string table (16MB max)
    uint32_t size      : 8;  // Function size in 16-byte units (max 4KB)
} __attribute__((packed)) kallsym_entry_t;

// At boot: entries are already sorted (build-time sort)
// Lookup: binary search over entries by address
```

**Build flow:**
1. `ld` links `jexos.bin` as usual
2. `tools/gen_kallsyms` reads `jexos.bin` with `-n`-style symbol listing, emits `kallsyms_data.S`
3. `kallsyms_data.S` is assembled and linked into `jexos.bin` (second link pass)
4. At boot, `kallsyms_init()` records the start/end of the embedded table

**Lookup:**

```c
const char* kallsyms_lookup(uint32_t addr, uint32_t* offset, uint32_t* size);
```

Binary search over the sorted table. Returns pointer to name in string table (or `"Unknown"`), and fills `*offset` (addr - func_start) and `*size`. Used in `dump_stack()` to print `arp_find+0x3a/0x8c` instead of bare hex.

**Memory cost:** ~8 bytes per exportable symbol. A kernel with ~800 C functions = ~6.4KB + string table. ~8-10KB total.

### Stack Guard Pages — Catch overflows immediately

**File:** `src/mm/paging.c` (modify `alloc_page` / fork path)
**File:** `src/kernel/panic.c` (modify page fault message)

Each kernel stack is 8KB allocated as 2 consecutive pages. The guard page is the page **below** the stack — unmapped in the page table:

```
stack top     0x....C000   ← ESP starts here (highest address)
stack         0x....A000   ← 8KB of stack space
guard page    0x....9000   ← UNMAPPED — access = page fault
```

In `fork()` / task creation, after allocating the stack:
```c
// Unmap the guard page right below the stack
// page_directory[VIRTUAL_TO_PDE(task->stack_bottom - PAGE_SIZE)] = 0;
```

In the page fault handler (`panic_handler`, int_no == 14):
```c
if (cr2 >= (uint32_t)(stack_bottom - 0x1000) && cr2 < (uint32_t)stack_bottom) {
    log_serial("*** STACK OVERFLOW ***\n");
    // Continue to normal panic display
}
```

This turns a silent data corruption into an immediate, obvious diagnostic.

---

## Phase 2: Logging Overhaul — pr_* system and vsnprintf

### Kernel vsnprintf

**Files:** `src/lib/vsnprintf.c`, `src/include/kernel/vsnprintf.h`

A minimal freestanding printf implementation supporting the formats you actually use:

```c
int vsnprintf(char* buf, size_t size, const char* fmt, va_list args);
int snprintf(char* buf, size_t size, const char* fmt, ...);
```

**Supported specifiers:**
- `%d`, `%i` — signed int
- `%u` — unsigned int
- `%x`, `%X` — hex (lower/upper)
- `%p` — pointer (always `0x` + 8 hex digits)
- `%s` — string
- `%c` — char
- `%%` — literal %
- `%ld`, `%lu`, `%lx` — long variants (same as non-l on 32-bit)

**Width and zero-padding:** `%08x` → `000000ff`, `%8x` → `       ff`, `%-8x` → `ff       `

**Not supported (YAGNI):** floating point, `%n`, `%ll` (no libgcc divdi3), positional args.

Implementation: ~100 lines of C. Iterates the format string, handles flags/width/padding, calls helper functions for each conversion.

### pr_* macros

**File:** `src/include/kernel/printk.h`

```c
#define LOG_EMERG   0
#define LOG_ALERT   1
#define LOG_CRIT    2
#define LOG_ERR     3
#define LOG_WARNING 4
#define LOG_NOTICE  5
#define LOG_INFO    6
#define LOG_DEBUG   7

extern int console_loglevel;
#define DEFAULT_CONSOLE_LOGLEVEL 6

#define pr_emerg(fmt, ...)   _printk(LOG_EMERG,   fmt, ##__VA_ARGS__)
#define pr_alert(fmt, ...)   _printk(LOG_ALERT,   fmt, ##__VA_ARGS__)
#define pr_crit(fmt, ...)    _printk(LOG_CRIT,    fmt, ##__VA_ARGS__)
#define pr_err(fmt, ...)     _printk(LOG_ERR,     fmt, ##__VA_ARGS__)
#define pr_warn(fmt, ...)    _printk(LOG_WARNING, fmt, ##__VA_ARGS__)
#define pr_notice(fmt, ...)  _printk(LOG_NOTICE,  fmt, ##__VA_ARGS__)
#define pr_info(fmt, ...)    _printk(LOG_INFO,    fmt, ##__VA_ARGS__)
#define pr_debug(fmt, ...)   _printk(LOG_DEBUG,   fmt, ##__VA_ARGS__)

/* Subsystem tag — define per file before including printk.h */
// #define pr_fmt(fmt) "[RTL8139] " fmt
```

### _printk — Core function

**File:** `src/kernel/printk.c`

```c
void _printk(int level, const char* fmt, ...)
{
    if (level > console_loglevel) return;

    char buf[256];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    log_serial(buf);

    if (!in_isr())   // needs a way to detect ISR context
        terminal_writestring(buf);

    klog_write(level, buf);
}
```

**ISR detection:** Either a per-CPU flag set in the interrupt entry stub, or a global `volatile int in_isr_context` incremented/decremented in the ISR wrapper.

### Runtime log level control

```c
// Shell command: kern.log_level=3  → only errors+
//               kern.log_level=7  → everything including debug
void cmd_kern_log_level(int level) {
    if (level >= 0 && level <= 7) console_loglevel = level;
}
```

### Conversion of existing code

All existing `log_serial()` + `terminal_writestring()` pairs (and `format_hex()` ad-hoc calls) are converted to use `pr_*` macros. This is a mechanical transformation touching every file.

### Log level transition

Add a new `dmesg -l level` flag to filter the kernel ring buffer dump by minimum severity.

---

## Phase 3: Driver Model — PCI auto-probe, initcalls, struct device

### Architecture

```
struct device          — what a device IS (.name, .irq, .io_base, .mmio, .driver_data)
struct device_driver   — what a driver IS  (.name, .probe, .remove)
struct pci_driver      — PCI-specific driver (.id_table, .probe, .remove)
struct bus_type        — how buses work   (.name, .match)
```

### Initcalls — Automatic driver discovery

**Modified:** `linker.ld` — add `.initcalls` section between `__initcall_start` / `__initcall_end`

```c
// include/kernel/init.h
typedef void (*initcall_t)(void);

#define __init __attribute__((section(".initcalls")))

#define early_init(fn)  static initcall_t __init_##fn __init = fn
#define device_init(fn) static initcall_t __init_##fn __init = fn
```

Two initcall levels:
- `early_init()` — arch-level setup (GDT, IDT, PIC, timer) — runs first
- `device_init()` — driver probing (PCI scan, RTL8139, IDE) — runs second

```c
// kernel/main.c — boot sequence:
void initcalls_run(void)
{
    extern initcall_t __initcall_start[], __initcall_end[];
    for (initcall_t* fn = __initcall_start; fn < __initcall_end; fn++) {
        (*fn)();
    }
}
```

No manual list of driver inits to maintain.

### PCI Driver Registration

**Files:** `src/include/pci.h` (extend), `src/drivers/pci.c`

```c
// include/pci.h
struct pci_device_id {
    uint16_t vendor;
    uint16_t device;
    uint32_t driver_data;  // for private data
};

struct pci_driver {
    const char*                name;
    const struct pci_device_id* id_table;
    int  (*probe)(pci_device_t* dev);   // return 0 on success
    void (*remove)(pci_device_t* dev);
    struct pci_driver* next;             // linked list
};

// pci.c
static struct pci_driver* pci_drivers = NULL;

void pci_register_driver(struct pci_driver* drv)
{
    drv->next = pci_drivers;
    pci_drivers = drv;

    /* Probe against all existing devices */
    for (int bus = 0; bus < 256; bus++) {
        for (int slot = 0; slot < 32; slot++) {
            for (int func = 0; func < 8; func++) {
                pci_device_t dev;
                if (pci_find_device_on(bus, slot, func, &dev)) {
                    for (const struct pci_device_id* id = drv->id_table;
                         id->vendor != 0; id++) {
                        if (dev.vendor_id == id->vendor &&
                            dev.device_id == id->device) {
                            pr_info("pci: %s matches %s\n", drv->name, dev.name);
                            drv->probe(&dev);
                        }
                    }
                }
            }
        }
    }
}
```

### RTL8139 conversion

```c
// drivers/rtl8139.c
static struct pci_device_id rtl8139_ids[] = {
    { PCI_VENDOR_REALTEK, PCI_DEVICE_RTL8139, 0 },
    { 0, 0, 0 }  // terminator
};

static int rtl8139_probe(pci_device_t* pci_dev)
{
    io_base = pci_dev->bar0 & ~0x3;
    irq_num = pci_dev->irq_line;
    // ... rest of init_rtl8139() ...
    return 0;
}

static struct pci_driver rtl8139_pci_driver = {
    .name     = "rtl8139",
    .id_table = rtl8139_ids,
    .probe    = rtl8139_probe,
};

static void rtl8139_init(void)
{
    pci_register_driver(&rtl8139_pci_driver);
}
device_init(rtl8139_init);
```

### Generic device structure (optional, for /sys/devices later)

```c
// include/device.h
struct device {
    const char* name;
    uint32_t    irq;
    uint32_t    io_base;
    void*       mmio_base;
    void*       driver_data;   // private per-driver data
    struct device* next;       // linked list for /sys enumeration
};
```

Drivers can call `device_register()` to add entries to the global device list. Enables `nicregs` to find devices by iterating the list rather than knowing them by name.

---

## Phase 4: devtmpfs + Filesystem Layout

### devtmpfs — Virtual filesystem for kernel state

**Files:** `src/fs/devtmpfs.c`, `src/include/fs/devtmpfs.h`

A new filesystem type that stores nothing on disk. Every `read()` calls a kernel function to populate a buffer; every `write()` dispatches to a handler.

```c
// include/fs/devtmpfs.h
struct devtmpfs_file {
    const char* name;
    int (*read)(char* buf, int max_len);    // populate buf, return bytes written
    int (*write)(const char* buf, int len); // process write data
    mode_t mode;                             // 0444, 0644, etc.
};

int devtmpfs_add_file(const char* path, struct devtmpfs_file* file);
int devtmpfs_init(void);                     // register with VFS
```

**Directory structure:**

```
/sys/
  kernel/
    version        → "JexOS v0.1 (gcc 14.2, commit abc123, Jun 10 2026)\n"
    log_level      → read: current console_loglevel; write: update it
    ticks          → "%u\n", system_ticks
  mm/
    heap           → "bump: 0x%x / %u blocks\n", kmalloc_pos, ... 
    free_pages     → "%u\n", free_page_count
  net/
    arp            → formatted ARP cache dump
    stats          → "tx=%u rx=%u\n", tx_count, rx_count
    mac            → "00:11:22:33:44:55\n"
  debug/
    backtrace      → read: dumps current call stack
```

**Adding a new file:**
```c
devtmpfs_add_file("/sys/net/arp", &(struct devtmpfs_file){
    .name  = "arp",
    .read  = arp_read_fn,
    .mode  = 0444,
});
```

**VFS integration:**
- `fs_open("/sys/net/arp")` → detects the path starts with `/sys/` → routes to devtmpfs
- `fs_read(fd, buf, len)` → devtmpfs calls the stored read handler, copies result to user buffer
- `fs_write(fd, buf, len)` → devtmpfs calls the write handler

**VFS rework required:** `fs_open()` currently calls `jexfs_open()` directly. devtmpfs needs a path-prefix dispatch. The VFS layer is modified to support multiple mounted filesystems:

```c
// src/fs/fs.c — revised fs_open()
struct mount_point {
    const char* path;           // "/sys", "/" etc.
    int path_len;
    int (*open)(const char* path, int flags);
    // read/write/... callbacks as needed
};

#define MAX_MOUNTS 4
static struct mount_point mounts[MAX_MOUNTS];
static int mount_count = 0;

int fs_mount(const char* path, const char* fstype)
{
    // Register a filesystem at the given mount point
}

int fs_open(const char* filename, int flags)
{
    // Walk mounts, longest prefix match wins
    for (int i = mount_count - 1; i >= 0; i--) {
        if (strncmp(filename, mounts[i].path, mounts[i].path_len) == 0)
            return mounts[i].open(filename, flags);
    }
    return -1;
}
```

### Filesystem Layout

```
/                  ← root (JexFS on disk)
  home/
    user/          ← default CWD when shell starts (created at boot)
  sys/             ← devtmpfs mount point (virtual filesystem)
```

At boot, after `jexfs_init()`:
```c
void fs_init_after(void)
{
    // Create /home/user/ if it doesn't exist
    if (jexfs_open("/home") < 0)
        jexfs_mkdir("/home");
    if (jexfs_open("/home/user") < 0)
        jexfs_mkdir("/home/user");
    // Mount devtmpfs
    devtmpfs_init();
    // Set CWD to home
    cwd_inode = jexfs_open("/home/user");
}
```

Shell starts at `~/` (`/home/user`). Crash dumps and user-created files land there. The prompt reflects this:
```
JexOS:~$ _
```

---

## Phase 5: Concurrency — Workqueues, Lockdep

### Workqueue — Deferred execution

**Files:** `src/kernel/workqueue.c`, `src/include/kernel/workqueue.h`

A linked list of work items pending execution in shell context (interrupts enabled, no ISR constraints).

```c
struct work {
    work_func_t func;
    void*       data;
    struct work* next;
};

void schedule_work(struct work* w);  // safe from ISR
void workqueue_run(void);            // call from main shell loop
```

**ISR safety:**
```c
static struct work* work_head = NULL;
static struct work* work_tail = NULL;

void schedule_work(struct work* w)
{
    uint32_t eflags;
    __asm__ volatile("pushf; pop %0" : "=r"(eflags));
    __asm__ volatile("cli");

    w->next = NULL;
    if (work_tail)
        work_tail->next = w;
    else
        work_head = w;
    work_tail = w;

    if (eflags & 0x200)
        __asm__ volatile("sti");
}
```

**Usage pattern:** ISR queues lightweight work items. Shell loop drains them:

```c
// rtl8139_handler now:
void rtl8139_handler(registers_t* regs)
{
    uint16_t status = inw(io_base + RTL8139_REG_ISR);
    outw(io_base + RTL8139_REG_ISR, status);
    if (status & RTL8139_ISR_ROK) {
        struct work* w = kmalloc(sizeof(struct work));
        w->func = net_drain_rx;  // poll ring, process packets
        w->data = NULL;
        schedule_work(w);
    }
}

// shell main loop:
while (1) {
    workqueue_run();
    // ... read input, execute commands ...
    __asm__ volatile("hlt");
}
```

This replaces the ad-hoc `rtl8139_poll_rx()` call sites scattered across DNS/TCP/Ping.

### Lockdep — Spinlock ordering detection

**Files:** `src/kernel/lockdep.c`, `src/include/kernel/lockdep.h`

A lightweight lock-order tracker inspired by Linux's lockdep, simplified for single-core operation.

```c
void lockdep_acquire(const char* name, void* addr);
void lockdep_release(void* addr);

#define spin_lock(lock, name) do { \
    lockdep_acquire(name, &(lock)); \
    /* actual spin_lock logic */ \
} while(0)

#define spin_unlock(lock) do { \
    lockdep_release(&(lock)); \
    /* actual spin_unlock logic */ \
} while(0)
```

**Data structure:**

```c
#define LOCKDEP_MAX_LOCKS 32
#define LOCKDEP_DEPTH_MAX 8

static struct {
    void*       addr;
    const char* name;
} lockdep_map[LOCKDEP_MAX_LOCKS];
static int lockdep_map_count = 0;

static void* lockdep_stack[LOCKDEP_DEPTH_MAX];
static int lockdep_depth = 0;
```

**Algorithm:**
- On `lockdep_acquire()`: for each lock currently held (lockdep_stack), register an edge (held → new). If the reverse edge already exists (new → held), `WARN_ON("Possible lock inversion detected")`.
- On `lockdep_release()`: pop from the stack.
- Edges stored as bitmask: `uint32_t edges[LOCKDEP_MAX_LOCKS]` — bit N set means "lock M is taken after N."

At boot, lockdep is enabled and its output goes to serial (since by the time it triggers, we're still debugging). If no lock issues are ever found in a given build, lockdep can be compiled out via `#ifdef CONFIG_LOCKDEP`.

---

## Phase 6: GDB Stub + ftrace-lite

### GDB Stub over Serial

**Files:** `src/debug/gdb_stub.c`, `src/include/debug/gdb_stub.h`

A GDB Remote Serial Protocol handler communicating over COM1. Triggered by `int3` (interrupt 3) or by calling `gdb_breakpoint()`.

**Entry point:**
```c
// Hook into interrupt 3 handler:
void gdb_stub_handler(registers_t* regs)
{
    serial_setup();  // ensure COM1 is configured
    gdb_stub_loop(regs);
}

// Called from any code to drop into GDB:
void gdb_breakpoint(void)
{
    __asm__ volatile("int3");
}
```

**GDB Remote Protocol — minimum commands:**

| Command | Input | Response |
|---------|-------|----------|
| `?` | No args | `S05` (signal SIGTRAP) |
| `g` | No args | All registers packed as hex string (32 bytes → 64 hex chars) |
| `G` | Hex register data | `OK` |
| `m addr,len` | Address + length | Hex memory dump |
| `M addr,len` | Address + length + hex data | `OK` |
| `c` | Optional addr | Continue execution (return from stub) |
| `s` | Optional addr | Single-step (set TF, resume) |
| `Z0,addr,kind` | Breakpoint address | `OK` (write `0xCC` at addr, save original) |
| `z0,addr,kind` | Breakpoint address | `OK` (restore original byte) |
| `k` | No args | Reboot |

**Breakpoint management:**

```c
#define MAX_BREAKPOINTS 256
static uint8_t bp_orig_bytes[MAX_BREAKPOINTS];   // saved original bytes
static uint32_t bp_addrs[MAX_BREAKPOINTS];
static int bp_count = 0;

// Z0: save byte at addr, replace with 0xCC (int3)
// When int3 fires: EIP points AFTER the int3 byte.
//   Restore the original byte, back up EIP by 1, then enter GDB command loop.
// On 'c' or 's': re-insert all breakpoints, then resume.
```

**Packet format:**
```
$ data # checksum    ← GDB sends
+                    ← ACK from stub
$ reply # checksum   ← Stub replies
-                    ← NAK (stub sends it, GDB retransmits)
```

Checksum: modulo 256 of all bytes between `$` and `#`. Reply loop: read chars into a buffer, validate checksum, ACK, dispatch, send response.

**Single-stepping:**
```c
void gdb_single_step(registers_t* regs)
{
    regs->eflags |= 0x100;  // Set Trap Flag
    // On next instruction, interrupt 1 fires → check if it's our TF → re-enter stub
}
```

Hook interrupt 1 in the IDT to check for our single-step (vs. other debug exceptions).

### ftrace-lite — Dynamic function tracer

**Files:** `src/debug/ftrace.c`, `src/include/debug/ftrace.h`

Uses GCC's `-finstrument-functions` to insert tracer hooks at every function entry/exit. Only active when enabled (single branch check — negligible overhead when off).

**Ring buffer:**

```c
#define FTRACE_BUF_SIZE 4096

#define FTRACE_ENTRY 1
#define FTRACE_EXIT  2

typedef struct {
    uint32_t func;       // address of called function
    uint32_t caller;     // address of call site
    uint32_t ticks;      // system tick at time of call
    uint32_t type  : 8;  // FTRACE_ENTRY or FTRACE_EXIT
} __attribute__((packed)) ftrace_record_t;

static ftrace_record_t ftrace_buf[FTRACE_BUF_SIZE];
static volatile uint32_t ftrace_head = 0;
static volatile int ftrace_enabled = 0;
```

**Filtering:**

```c
// Trace all functions whose names contain "arp" or "net_send"
// Filter list: simple string prefix/substring match
#define FTRACE_MAX_FILTERS 16
static char ftrace_filters[FTRACE_MAX_FILTERS][32];
static int ftrace_filter_count = 0;

// Defined in ftrace.c (linked only when building with ftrace):
void __cyg_profile_func_enter(void* func, void* call_site)
{
    if (!ftrace_enabled) return;
    uint32_t idx = ftrace_head;
    ftrace_buf[idx].func   = (uint32_t)func;
    ftrace_buf[idx].caller = (uint32_t)call_site;
    ftrace_buf[idx].ticks  = system_ticks;
    ftrace_buf[idx].type   = FTRACE_ENTRY;
    ftrace_head = (idx + 1) % FTRACE_BUF_SIZE;
}

void __cyg_profile_func_exit(void* func, void* call_site)
{
    if (!ftrace_enabled) return;
    // ... similar, type = FTRACE_EXIT ...
}
```

**Shell interface:**
```
ftrace start          ← enable tracing
ftrace stop           ← disable
ftrace add arp        ← filter: only trace functions matching "arp"
ftrace add net_send   ← filter: only trace functions matching "net_send"
ftrace clear          ← clear filters
ftrace dump           ← dump ring buffer with kallsyms names
```

**Build integration:**
```makefile
# Add to CFLAGS when ftrace is enabled:
CFLAGS += -finstrument-functions
```

**Compile-out when not needed:**
```c
// When !CONFIG_FTRACE, provide empty stubs (or use weak aliases)
// so the kernel links without -finstrument-functions normally.
```

---

## Phase 7: Shell Quality of Life

### Tab Completion

**File:** `src/bin/shell.c` (modify input loop)

```c
#define SHELL_BUILTINS \
    "ping", "arp", "route", "dump", "dmesg", "ps", "kill", \
    "uptime", "bt", "backtrace", "nicregs", "heapcheck", \
    "stackcheck", "runtests", "tcpdump", "fetch", \
    "ls", "cat", "rm", "reboot", "ftrace", "help", \
    "top", "history", "kern.log_level"

// On TAB press:
void shell_tab_complete(char* buf, int* pos)
{
    // 1. Find what the user has typed so far (last word)
    // 2. Scan builtins + filesystem (ls root) for prefix matches
    // 3. If one match: complete the word
    // 4. If multiple: print them, re-display prompt + buf
}
```

### Command History

**File:** `src/bin/shell.c`

Circular buffer of 16 commands. Up/down arrows recall.

```c
#define HIST_SIZE 16
static char history[HIST_SIZE][256];
static int hist_head = 0, hist_count = 0, hist_pos = 0;

void hist_add(const char* cmd) {
    strcpy(history[hist_head], cmd);
    hist_head = (hist_head + 1) % HIST_SIZE;
    hist_count = min(hist_count + 1, HIST_SIZE);
    hist_pos = hist_count;
}

// On UP arrow: hist_pos--; strcpy(buf, history[hist_pos % HIST_SIZE])
// On DOWN arrow: hist_pos++; if at end, clear buf
```

### `top` Command — CPU Usage Per Task

**File:** `src/bin/shell.c` (add top_command)
**Modified:** `src/drivers/timer.c` (track ticks per task)

```c
// timer.c — on each tick, increment current task's tick counter:
void timer_tick(void)
{
    if (current_task)
        current_task->cpu_ticks++;
    system_ticks++;
}

// shell command:
void top_command(void)
{
    uint32_t total = 0;
    for each task:
        total += task->cpu_ticks;

    terminal_writestring("PID  NAME       CPU%   STATE\n");
    for each task:
        uint32_t pct = (task->cpu_ticks * 100) / total;
        print(" %-3d %-10s %-5u %s\n", task->pid, task->name, pct, state_str);
}
```

Adds a `cpu_ticks` field to the task struct.

### Boot Banner

**File:** `src/kernel/kernel.c` (modify kmain)

```c
// At the start of kmain, after initcalls_run:
void print_banner(void)
{
    terminal_writestring("JexOS v0.1 — i386 — Monolithic\n");
    terminal_writestring("Build: " __DATE__ " " __TIME__ "\n");

    uint32_t mem_size;
    __asm__ volatile("int $0x12" : "=ax"(mem_size));  // BIOS memory query
    char buf[32];
    snprintf(buf, sizeof(buf), "RAM: %u KB\n", mem_size);
    terminal_writestring(buf);

    terminal_writestring("Device: RTL8139");
    // MAC address from rtl8139_get_mac()
    terminal_writestring(" (");
    print_mac(mac);
    terminal_writestring(")\n");

    terminal_writestring("Device: PIIX4 IDE\n");
    terminal_writestring("FS: JexFS v1, 1.44 MB\n");
    terminal_writestring("\n");
    terminal_writestring("shell@jex:~$ ");
}
```

### Color Prompt (optional, if terminal supports ANSI)

```c
#define ANSI_GREEN  "\x1b[32m"
#define ANSI_RESET  "\x1b[0m"
#define ANSI_BLUE   "\x1b[34m"
#define PROMPT ANSI_GREEN "shell@jex" ANSI_RESET ":" ANSI_BLUE "~" ANSI_RESET "$ "
```

---

## Implementation Order & Dependencies

```
Phase 1: Dev Tools           → No deps, start here
Phase 2: Logging + vsnprintf → No deps, parallel with P1
Phase 3: Driver Model        → No deps, parallel with P1/P2
Phase 4: devtmpfs + Layout   → Depends on Phase 3 for /sys/devices/ support (optional)
Phase 5: Workqueue + Lockdep → Depends on Phase 1 (WARN_ON for lockdep)
Phase 6: GDB Stub + ftrace   → Depends on Phase 1 (kallsyms for trace output)
Phase 7: Shell QoL           → No deps, can be interleaved anywhere
```

Actual recommended build order:
```
P1 → P2 → P7 → P3 → P4 → P5 → P6
```

This front-loads the debugging improvements (WARN_ON, dump_stack, vsnprintf, pr_*) so the harder changes (driver model, workqueues, GDB stub) are developed using the improved tools.

## Risk Assessment

| Risk | Mitigation |
|------|-----------|
| kallsyms adds 8-10KB to kernel | Required only for dev builds; can be compiled out |
| -finstrument-functions doubles function size | ftrace defaults to OFF; compile without it for production |
| GDB stub serial protocol conflicts with normal serial output | GDB stub takes over COM1 while active; `gdb_breakpoint()` is a deliberate call |
| Driver model restructure breaks existing device shutdown | Every driver gets a parallel `probe()` path; old `init_*()` functions remain until all drivers are converted |
| Multiple initcall priorities cause boot-order bugs | Only 2 levels (early/device) — no complex ordering |
| Stack guards reduce available address space | 1 unmapped page per task on a 4GB address space — not a concern |
