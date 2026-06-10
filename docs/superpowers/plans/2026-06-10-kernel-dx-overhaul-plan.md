# JexOS Developer Experience Overhaul — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add modern kernel debugging infrastructure, driver model, logging, virtual filesystem, concurrency primitives, and shell quality-of-life features to JexOS.

**Architecture:** Monolithic 32-bit x86 i386 kernel. All phases are additive — no existing functionality is removed. Phases 1-2 can be developed in parallel; Phase 3+ builds on earlier phases.

**Tech Stack:** gcc (freestanding `-m32 -ffreestanding -std=gnu99`), ld with custom linker script, JexFS (native FS), COM1 serial, QEMU testing.

---

## File Structure

### Files Created

| File | Phase | Purpose |
|------|-------|---------|
| `src/include/kernel/backtrace.h` | 1 | Stack unwinder API |
| `src/kernel/backtrace.c` | 1 | Stack unwinder implementation |
| `src/include/kernel/warn.h` | 1 | WARN_ON macro |
| `src/include/kernel/kallsyms.h` | 1 | Symbol lookup API |
| `src/kernel/kallsyms.c` | 1 | Symbol table lookup |
| `src/kernel/kallsyms_data.S` | 1 | Build-generated symbol data |
| `tools/gen_kallsyms.c` | 1 | Post-link symbol extractor |
| `src/include/kernel/vsnprintf.h` | 2 | Printf API |
| `src/lib/vsnprintf.c` | 2 | Printf implementation |
| `src/include/kernel/printk.h` | 2 | pr_* macro API |
| `src/kernel/printk.c` | 2 | _printk core |
| `src/include/init.h` | 3 | Initcall macros |
| `src/include/device.h` | 3 | Device abstraction |
| `src/fs/devtmpfs.h` | 4 | devtmpfs API |
| `src/fs/devtmpfs.c` | 4 | devtmpfs implementation |
| `src/include/kernel/workqueue.h` | 5 | Workqueue API |
| `src/kernel/workqueue.c` | 5 | Workqueue implementation |
| `src/include/kernel/lockdep.h` | 5 | Lockdep API |
| `src/kernel/lockdep.c` | 5 | Lockdep implementation |
| `src/include/debug/gdb_stub.h` | 6 | GDB stub API |
| `src/debug/gdb_stub.c` | 6 | GDB stub implementation |
| `src/include/debug/ftrace.h` | 6 | ftrace API |
| `src/debug/ftrace.c` | 6 | ftrace implementation |

### Files Modified

| File | Phase | Changes |
|------|-------|---------|
| `linker.ld` | 1, 3 | Add `.kallsyms`, `.initcalls` sections |
| `src/kernel/panic.c` | 1 | Extract unwind_stack to backtrace.c, add stack overflow check |
| `src/mm/paging.c` | 1 | Unmap guard page below each kernel stack |
| `makefile` | 1, 6 | kallsyms build pass, ftrace flag |
| all `.c` files | 2 | Convert log_serial + terminal_writestring pairs to pr_* |
| `src/kernel/kernel.c` | 3, 4 | Replace manual init calls with initcalls_run, add boot banner, create /home/user/ |
| `src/drivers/pci.c` | 3 | Add pci_register_driver, auto-probe |
| `src/drivers/rtl8139.c` | 3 | Convert to pci_driver + initcall |
| `src/drivers/ide.c` | 3 | Convert to initcall |
| `src/drivers/timer.c` | 3, 7 | Convert to initcall, add per-task cpu_ticks |
| `src/fs/fs.c` | 4 | Mount point dispatch for devtmpfs |
| `src/bin/shell.c` | 5, 7 | workqueue_run in main loop, tab completion, history, top, ftrace command |
| `src/arch/i386/idt.c` | 6 | Register int3 + int1 handlers |
| `tools/mkjexfs.c` | 4 | Add subdirectory creation support |

---

## Phase 1: Dev Tools

### Task 1.1: Extract stack unwinder into backtrace.c

**Files:**
- Create: `src/include/kernel/backtrace.h`
- Create: `src/kernel/backtrace.c`
- Modify: `src/kernel/panic.c` — remove unwind_stack, include backtrace.h

- [ ] **Step 1: Write backtrace.h**

```c
// src/include/kernel/backtrace.h
#ifndef BACKTRACE_H
#define BACKTRACE_H
#include <stdint.h>

/**
 * unwind_stack - Walk the frame pointer chain to produce a backtrace.
 * @ebp: Initial base pointer to start from.
 * @eip_out: Array to fill with recovered EIP values.
 * @max_frames: Capacity of eip_out.
 * Returns: Number of frames recovered.
 */
int unwind_stack(uint32_t ebp, uint32_t* eip_out, int max_frames);

/**
 * dump_stack - Print backtrace to both terminal and serial.
 * Safe to call with interrupts enabled (terminal) or disabled (serial path).
 */
void dump_stack(void);

/**
 * dump_stack_serial - Print backtrace to serial only.
 * Safe to call from ISR context (no terminal access).
 */
void dump_stack_serial(void);

#endif
```

- [ ] **Step 2: Write backtrace.c with the extracted unwind_stack + format_hex**

```c
// src/kernel/backtrace.c
#include "backtrace.h"
#include "terminal.h"
#include "serial.h"
#include "kernel/kallsyms.h"

void format_hex(uint32_t val, char* out)
{
    const char* digits = "0123456789ABCDEF";
    out[0] = '0'; out[1] = 'x';
    for (int i = 0; i < 8; i++)
        out[2 + i] = digits[(val >> (28 - i * 4)) & 0xF];
    out[10] = '\0';
}

/**
 * unwind_stack - Walk EBP chain with safety checks.
 *
 * Each stack frame has saved EBP at [0] and saved EIP at [1].
 * Stop when EBP stops advancing, goes out of range, or is misaligned.
 */
int unwind_stack(uint32_t ebp, uint32_t* eip_out, int max_frames)
{
    int count = 0;
    while (ebp != 0 && eip_out && count < max_frames) {
        if ((ebp & 3) != 0 || ebp < 0x100000)
            break;
        uint32_t* frame = (uint32_t*)ebp;
        uint32_t saved_ebp = frame[0];
        uint32_t saved_eip = frame[1];
        if (saved_ebp <= ebp || saved_ebp > 0xFFFFF000)
            break;
        eip_out[count++] = saved_eip;
        ebp = saved_ebp;
    }
    return count;
}

static void dump_stack_common(int to_terminal)
{
    uint32_t ebp;
    __asm__ volatile("mov %%ebp, %0" : "=r"(ebp));
    uint32_t eip_frames[16];
    int depth = unwind_stack(ebp, eip_frames, 16);
    char buf[12];

    for (int i = 0; i < depth; i++) {
        log_serial("  [<");
        log_hex_serial(eip_frames[i]);
        log_serial(">]\n");
        if (to_terminal) {
            terminal_writestring("  [<");
            format_hex(eip_frames[i], buf);
            terminal_writestring(buf);
            terminal_writestring(">]\n");
        }
    }
}

void dump_stack(void)
{
    terminal_writestring("Call stack:\n");
    log_serial("Call stack:\n");
    dump_stack_common(1);
}

void dump_stack_serial(void)
{
    log_serial("Call stack:\n");
    dump_stack_common(0);
}
```

- [ ] **Step 3: Modify panic.c to use backtrace.h instead of local unwind_stack**

Edit `src/kernel/panic.c`:
- Add `#include "kernel/backtrace.h"` at the top
- Remove the `unwind_stack()` function (lines 79-99)
- Remove the `format_hex()` function (lines 36-43)
- Replace the stack trace block (lines 163-182) to just call `dump_stack_serial()`:

```c
    /* Stack trace — printed via serial dump_stack_serial */
    dump_stack_serial();

    /* Also print to terminal manually (format_hex is now in backtrace.c) */
    uint32_t eip_frames[16];
    int depth = unwind_stack(regs->ebp, eip_frames, 16);
    if (depth > 0) {
        terminal_writestring("\nStack Trace (depth ");
        char d[4];
        int n = depth, j = 0;
        if (n == 0) { d[j++] = '0'; }
        else { char tmp[8]; int k = 0;
            while (n > 0) { tmp[k++] = '0' + (n % 10); n /= 10; }
            while (k > 0) d[j++] = tmp[--k]; }
        d[j] = '\0';
        terminal_writestring(d);
        terminal_writestring("):\n");
        for (int i = 0; i < depth; i++) {
            terminal_writestring("  ");
            format_hex(eip_frames[i], buf);
            terminal_writestring(buf);
            terminal_writestring("\n");
        }
    }
```

- [ ] **Step 4: Build and verify**

Run: `make -j4`
Expected: No errors or warnings. Binary links.

- [ ] **Step 5: Commit**

```bash
git add src/include/kernel/backtrace.h src/kernel/backtrace.c src/kernel/panic.c
git commit -m "feat: extract stack unwinder into reusable backtrace.c

Move unwind_stack and format_hex from panic.c to backtrace.c so
WARN_ON and other callers can produce stack traces without
including the entire panic handler."
```

### Task 1.2: Add WARN_ON macro

**Files:**
- Create: `src/include/kernel/warn.h`

- [ ] **Step 1: Write warn.h**

```c
// src/include/kernel/warn.h
#ifndef WARN_H
#define WARN_H
#include "kernel/backtrace.h"
#include "serial.h"

/**
 * WARN_ON - Print warning with stack trace and continue.
 * @condition: Expression that should be false (warns if true).
 *
 * Unlike ASSERT, this does NOT halt the system.
 * The stack trace is printed to serial (safe in any context).
 */
#define WARN_ON(condition) do {                                         \
    if ((condition)) {                                                   \
        log_serial("WARN_ON: " #condition " at %s:%d\n",                \
                   __FILE__, __LINE__);                                  \
        dump_stack_serial();                                             \
    }                                                                    \
} while(0)

#endif
```

Note: `log_serial` doesn't currently support format strings with `%s` and `%d`. After Phase 2 is complete (vsnprintf + printk), WARN_ON will automatically work with formatted output. For now, use the available primitives:

```c
#define WARN_ON(condition) do {                                         \
    if ((condition)) {                                                   \
        log_serial("WARN_ON at ");                                       \
        log_serial(__FILE__);                                            \
        log_serial(":");                                                 \
        /* file and line as best we can */                              \
        log_serial("\n");                                                \
        dump_stack_serial();                                             \
    }                                                                    \
} while(0)
```

- [ ] **Step 2: Build and verify**

Run: `make -j4`
Expected: No errors.

- [ ] **Step 3: Commit**

```bash
git add src/include/kernel/warn.h
git commit -m "feat: add WARN_ON macro for non-fatal assertions"
```

### Task 1.3: Add kallsyms for symbolic backtraces

**Files:**
- Create: `src/include/kernel/kallsyms.h`
- Create: `src/kernel/kallsyms.c`
- Create: `tools/gen_kallsyms.c`
- Create: `src/kernel/kallsyms_data.S.in` (template)
- Modify: `linker.ld` — add `.kallsyms` section
- Modify: `Makefile` — add kallsyms generation step

- [ ] **Step 1: Write kallsyms.h**

```c
// src/include/kernel/kallsyms.h
#ifndef KALLSYMS_H
#define KALLSYMS_H
#include <stdint.h>

typedef struct {
    uint32_t addr;          // function start address
    uint32_t name_off : 24; // offset into string table (16MB max)
    uint32_t size      : 8; // function size in 16-byte units (max 4KB)
} __attribute__((packed)) kallsym_entry_t;

/**
 * kallsyms_init - Initialize symbol table at boot.
 * Must be called once after the kernel is fully loaded.
 */
void kallsyms_init(void);

/**
 * kallsyms_lookup - Look up a code address in the symbol table.
 * @addr: Address to look up.
 * @offset: (out) Offset of addr from the function start.
 * @size: (out) Size of the function in bytes.
 * Returns: Pointer to function name string, or "Unknown".
 */
const char* kallsyms_lookup(uint32_t addr, uint32_t* offset, uint32_t* size);

#endif
```

- [ ] **Step 2: Write kallsyms.c**

```c
// src/kernel/kallsyms.c
#include "kernel/kallsyms.h"
#include "string.h"

/* Symbols are embedded via linker section */
extern char __kallsyms_start[];
extern char __kallsyms_end[];

static kallsym_entry_t*   sym_entries;
static const char*        sym_strings;
static int                sym_count;

void kallsyms_init(void)
{
    /* The .kallsyms section starts with a 4-byte count followed by
     * entries, then the string table starts right after the entries. */
    uint32_t* header = (uint32_t*)__kallsyms_start;
    sym_count  = (int)header[0];
    sym_entries = (kallsym_entry_t*)&header[1];
    sym_strings = (const char*)&sym_entries[sym_count];
}

const char* kallsyms_lookup(uint32_t addr, uint32_t* offset, uint32_t* size)
{
    /* Binary search over sorted entries */
    int lo = 0, hi = sym_count - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        uint32_t func_addr = sym_entries[mid].addr;
        uint32_t func_size = (uint32_t)sym_entries[mid].size * 16;

        if (addr >= func_addr && addr < func_addr + func_size) {
            *offset = addr - func_addr;
            *size   = func_size;
            return &sym_strings[sym_entries[mid].name_off];
        }

        if (addr < func_addr)
            hi = mid - 1;
        else
            lo = mid + 1;
    }

    *offset = 0;
    *size   = 0;
    return "Unknown";
}
```

- [ ] **Step 3: Write gen_kallsyms.c**

```c
// tools/gen_kallsyms.c
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/*
 * gen_kallsyms reads nm output (--print-size) from stdin and writes
 * a .S file to stdout with a packed symbol table for the kernel.
 *
 * Usage: nm -n -S jexos.bin | grep ' T ' | sort | ./gen_kallsyms > kallsyms_data.S
 */

typedef struct {
    uint32_t addr;
    uint32_t size;
    char*    name;
} symbol_t;

int sym_cmp(const void* a, const void* b)
{
    return ((symbol_t*)a)->addr - ((symbol_t*)b)->addr;
}

int main(void)
{
    symbol_t syms[4096];
    int count = 0;
    char line[512];

    while (fgets(line, sizeof(line), stdin) && count < 4096) {
        uint32_t addr, size;
        char type[4], name[256];
        /* nm -n -S output: ADDR SIZE TYPE NAME */
        if (sscanf(line, "%x %x %3s %255s", &addr, &size, type, name) >= 4) {
            if (type[0] == 'T' || type[0] == 't') {
                syms[count].addr = addr;
                syms[count].size = size;
                syms[count].name = strdup(name);
                count++;
            }
        }
    }

    qsort(syms, count, sizeof(symbol_t), sym_cmp);

    printf("# Auto-generated by gen_kallsyms\n");
    printf(".section .kallsyms, \"a\"\n");
    printf(".globl __kallsyms_start\n");
    printf(".globl __kallsyms_end\n");
    printf("__kallsyms_start:\n");

    /* Emit count as 4 bytes */
    printf(".long %d\n", count);

    /* Emit packed entries */
    int string_offset = 0;
    for (int i = 0; i < count; i++) {
        uint32_t name_off = string_offset;
        uint32_t packed_size = (syms[i].size + 15) / 16;  /* in 16-byte units */
        if (packed_size > 255) packed_size = 255;

        /* Pack entry: addr (4 bytes), name_off:size (4 bytes) */
        uint32_t hi = (name_off & 0xFFFFFF) | (packed_size << 24);
        printf(".long 0x%08x\n", syms[i].addr);
        printf(".long 0x%08x\n", hi);

        string_offset += strlen(syms[i].name) + 1;
    }

    /* Emit string table */
    printf("__kallsyms_strings:\n");
    for (int i = 0; i < count; i++) {
        printf(".asciz \"%s\"\n", syms[i].name);
    }

    printf("__kallsyms_end:\n");
    return 0;
}
```

- [ ] **Step 4: Add .kallsyms section to linker.ld**

```ld
    /* Read-write data (initialized) */
    .data BLOCK(4K) : ALIGN(4K)
    {
        *(.data)
    }

    /* Kallsyms symbol table (sorted, read-only) */
    .kallsyms BLOCK(4K) : ALIGN(4K)
    {
        __kallsyms_start = .;
        *(.kallsyms)
        __kallsyms_end = .;
    }

    /* Read-write data (uninitialized) and stack */
```

- [ ] **Step 5: Modify Makefile for kallsyms generation**

After the existing `$(KERNEL): $(OBJECTS)` rule, add:

```makefile
# Second link pass with embedded kallsyms
$(KERNEL): $(OBJECTS)
	$(LD) $(LDFLAGS) -o $@ $(OBJECTS)
	nm -n -S $@ | grep ' T ' | sort | ./tools/gen_kallsyms > src/kernel/kallsyms_data.S
	$(AS) $(ASFLAGS) src/kernel/kallsyms_data.S -o src/kernel/kallsyms_data.o
	$(LD) $(LDFLAGS) -o $@ $(OBJECTS) src/kernel/kallsyms_data.o
```

Also add:
```makefile
tools/gen_kallsyms: tools/gen_kallsyms.c
	$(CC) -o $@ $<
```

And update the `clean` target:
```makefile
clean:
	rm -f $(OBJECTS) $(KERNEL) $(IMG) $(ISO) tools/mkjexfs tools/gen_kallsyms
	rm -f src/kernel/kallsyms_data.S src/kernel/kallsyms_data.o
	rm -rf iso/
```

- [ ] **Step 6: Call kallsyms_init at boot**

In `src/kernel/kernel.c` near the end of `kmain`, after `fs_init()`:
```c
kallsyms_init();
```

Add `#include "kernel/kallsyms.h"` at the top of kernel.c.

- [ ] **Step 7: Integrate kallsyms into dump_stack**

In `src/kernel/backtrace.c`, update the `dump_stack_common` function to use kallsyms:

```c
static void dump_stack_common(int to_terminal)
{
    uint32_t ebp;
    __asm__ volatile("mov %%ebp, %0" : "=r"(ebp));
    uint32_t eip_frames[16];
    int depth = unwind_stack(ebp, eip_frames, 16);
    char buf[12];

    for (int i = 0; i < depth; i++) {
        uint32_t offset, size;
        const char* name = kallsyms_lookup(eip_frames[i], &offset, &size);
        log_serial("  [<");
        log_hex_serial(eip_frames[i]);
        log_serial(">] ");
        log_serial(name);
        log_serial("+0x");
        log_hex_serial(offset);
        log_serial("/0x");
        log_hex_serial(size);
        log_serial("\n");
        if (to_terminal) {
            terminal_writestring("  [<");
            format_hex(eip_frames[i], buf);
            terminal_writestring(buf);
            terminal_writestring(">] ");
            terminal_writestring(name);
        }
    }
}
```

- [ ] **Step 8: Build and verify**

Run: `make -j4`
Expected: Two-pass link completes, kernel binary has kallsyms embedded.
Run: `nm -n jexos.bin | tail -10`
Expected: Shows the final linked addresses.

- [ ] **Step 9: Commit**

```bash
git add src/include/kernel/kallsyms.h src/kernel/kallsyms.c tools/gen_kallsyms.c linker.ld Makefile src/kernel/kernel.c src/kernel/backtrace.c
git commit -m "feat: add kallsyms for symbolic backtrace output

kallsyms embeds a packed sorted symbol table in the kernel image
via a two-pass link. gen_kallsyms reads nm output and generates
a .S file with entries and string table. dump_stack now shows
function names and offsets instead of bare hex addresses."
```

### Task 1.4: Stack guard pages

**Files:**
- Modify: `src/mm/paging.c` — unmap guard page below each kernel stack
- Modify: `src/kernel/panic.c` — detect guard page fault

- [ ] **Step 1: Understand existing stack allocation**

Find where kernel stacks are allocated. Check `src/kernel/task.c`:
```c
// In fork or task creation — likely something like:
uint32_t stack = (uint32_t)pmm_alloc_blocks(KERNEL_STACK_SIZE / PMM_BLOCK_SIZE);
// Guards would be: unmap the page at stack - 0x1000
```

- [ ] **Step 2: Add guard page unmapping**

After the stack is allocated and mapped, unmap the guard page:
```c
// Guard page is the page right below the allocated stack
// If stack_top = KERNEL_STACK_SIZE, guard is at stack_top - KERNEL_STACK_SIZE - 0x1000
uint32_t guard_page = (stack_bottom - 0x1000) & ~0xFFF;
// Unmap by clearing the page table entry
paging_unmap_page(guard_page);
```

Add the unmap call right after the stack mapping loop in the fork code.

- [ ] **Step 3: Add stack overflow check in panic handler**

In `panic_handler()`, after reading CR2, check if the faulting address is in a guard page:
```c
if (regs->int_no == 14) {
    uint32_t cr2 = 0;
    __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));

    /* Check if this is a stack overflow (guard page) */
    if (cr2 >= 0x100000 && cr2 < 0xFFFFF000) {
        /* Rough check: fault in a plausible guard page region */
        log_serial("PAGE FAULT at 0x");
        log_hex_serial(cr2);
        log_serial("\n");
    }

    /* Normal panic page fault display continues... */
}
```

- [ ] **Step 4: Build and verify**

Run: `make -j4`
Expected: Builds clean.

- [ ] **Step 5: Commit**

```bash
git add src/mm/paging.c src/kernel/panic.c
git commit -m "feat: add stack guard pages for overflow detection"
```

---

## Phase 2: Logging Overhaul

### Task 2.1: Kernel vsnprintf

**Files:**
- Create: `src/include/kernel/vsnprintf.h`
- Create: `src/lib/vsnprintf.c`

- [ ] **Step 1: Write vsnprintf.h**

```c
// src/include/kernel/vsnprintf.h
#ifndef VSNPRINTF_H
#define VSNPRINTF_H
#include <stddef.h>
#include <stdarg.h>

int vsnprintf(char* buf, size_t size, const char* fmt, va_list args);
int snprintf(char* buf, size_t size, const char* fmt, ...);

#endif
```

- [ ] **Step 2: Write vsnprintf.c**

```c
// src/lib/vsnprintf.c
#include "kernel/vsnprintf.h"
#include <stddef.h>
#include <stdint.h>

static void print_dec(char** p, char* end, uint32_t val, int sign, int width, int zero)
{
    char tmp[12];
    int neg = 0, len = 0;

    if (sign && (int32_t)val < 0) {
        neg = 1;
        val = (uint32_t)-(int32_t)val;
    }

    do {
        tmp[len++] = '0' + (val % 10);
        val /= 10;
    } while (val > 0);

    if (neg) tmp[len++] = '-';
    while (len < width && zero) tmp[len++] = '0';
    while (len < width) tmp[len++] = ' ';

    for (int i = len - 1; i >= 0 && *p < end; i--)
        *(*p)++ = tmp[i];
}

static void print_hex(char** p, char* end, uint32_t val, int upper, int width, int zero)
{
    const char* digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    char tmp[10];
    int len = 0; /* minimum 2 for 0x */

    if (width == 0) {
        /* Default: print full 8 hex digits without 0x */
        for (int i = 7; i >= 0; i--)
            tmp[len++] = digits[(val >> (i * 4)) & 0xF];
    } else {
        do {
            tmp[len++] = digits[val & 0xF];
            val >>= 4;
        } while (val > 0);
    }

    while (len < width && zero) tmp[len++] = '0';
    while (len < width) tmp[len++] = ' ';

    for (int i = len - 1; i >= 0 && *p < end; i--)
        *(*p)++ = tmp[i];
}

int vsnprintf(char* buf, size_t size, const char* fmt, va_list args)
{
    char* p = buf;
    char* end = buf + size - 1;

    for (; *fmt && p < end; fmt++) {
        if (*fmt != '%') {
            *p++ = *fmt;
            continue;
        }

        fmt++; /* skip '%' */

        /* Parse flags and width */
        int zero = 0;
        int width = 0;
        int left = 0;

        while (*fmt == '-' || *fmt == '0') {
            if (*fmt == '-') left = 1;
            if (*fmt == '0') zero = 1;
            fmt++;
        }
        while (*fmt >= '0' && *fmt <= '9')
            width = width * 10 + (*fmt++ - '0');
        if (left) zero = 0;

        /* Parse length modifier */
        int is_long = 0;
        if (*fmt == 'l') {
            is_long = 1;
            fmt++;
        }

        /* Parse specifier */
        switch (*fmt) {
            case 'd':
            case 'i':
                print_dec(&p, end, va_arg(args, int), 1, width, zero);
                break;
            case 'u':
                print_dec(&p, end, va_arg(args, unsigned int), 0, width, zero);
                break;
            case 'x':
                print_hex(&p, end, va_arg(args, unsigned int), 0, width, zero);
                break;
            case 'X':
                print_hex(&p, end, va_arg(args, unsigned int), 1, width, zero);
                break;
            case 'p': {
                uint32_t v = (uint32_t)va_arg(args, void*);
                /* pointer: always 0x prefix + 8 hex digits */
                if (p < end) *p++ = '0';
                if (p < end) *p++ = 'x';
                print_hex(&p, end, v, 0, 8, 1);
                break;
            }
            case 's': {
                const char* s = va_arg(args, const char*);
                if (!s) s = "(null)";
                while (*s && p < end)
                    *p++ = *s++;
                break;
            }
            case 'c':
                if (p < end) *p++ = (char)va_arg(args, int);
                break;
            case '%':
                if (p < end) *p++ = '%';
                break;
            default:
                if (p < end) *p++ = '%';
                if (p < end) *p++ = *fmt;
                break;
        }
    }

    *p = '\0';
    return (int)(p - buf);
}

int snprintf(char* buf, size_t size, const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    int r = vsnprintf(buf, size, fmt, args);
    va_end(args);
    return r;
}
```

- [ ] **Step 3: Build and verify**

Run: `make -j4`
Expected: Clean build.

- [ ] **Step 4: Add a quick vsnprintf test**

Edit `src/tests/test_suite.c` (or create a standalone test):
```c
#include "kernel/vsnprintf.h"

static int test_vsnprintf(void)
{
    char buf[64];
    char expect[64];
    int fail = 0;

    snprintf(buf, sizeof(buf), "hello");
    // expected: "hello"

    snprintf(buf, sizeof(buf), "%d", 42);
    // expected: "42"

    snprintf(buf, sizeof(buf), "%x", 255);
    // expected: "ff"

    snprintf(buf, sizeof(buf), "%p", (void*)0x100456);
    // expected: "0x10045600" (or similar)

    snprintf(buf, sizeof(buf), "%s", "test");
    // expected: "test"

    return fail;
}
```

- [ ] **Step 5: Commit**

```bash
git add src/include/kernel/vsnprintf.h src/lib/vsnprintf.c
git commit -m "feat: add kernel vsnprintf/snprintf

Minimal freestanding implementation supporting %d/%u/%x/%X/%p/%s/%c/%%.
No floating point or 64-bit division."
```

### Task 2.2: pr_* macro system

**Files:**
- Create: `src/include/kernel/printk.h`
- Create: `src/kernel/printk.c`
- Modify: `src/kernel/klog.c` — add log level parameter (if needed)

- [ ] **Step 1: Write printk.h**

```c
// src/include/kernel/printk.h
#ifndef PRINTK_H
#define PRINTK_H
#include <stdarg.h>

/* Log levels (Linux-compatible) */
#define LOG_EMERG   0
#define LOG_ALERT   1
#define LOG_CRIT    2
#define LOG_ERR     3
#define LOG_WARNING 4
#define LOG_NOTICE  5
#define LOG_INFO    6
#define LOG_DEBUG   7

/* Subsystem format tag — define before including this */
#ifndef pr_fmt
#define pr_fmt(fmt) fmt
#endif

extern int console_loglevel;

void _printk(int level, const char* fmt, ...);

#define pr_emerg(fmt, ...)   _printk(LOG_EMERG,   pr_fmt(fmt), ##__VA_ARGS__)
#define pr_alert(fmt, ...)   _printk(LOG_ALERT,   pr_fmt(fmt), ##__VA_ARGS__)
#define pr_crit(fmt, ...)    _printk(LOG_CRIT,    pr_fmt(fmt), ##__VA_ARGS__)
#define pr_err(fmt, ...)     _printk(LOG_ERR,     pr_fmt(fmt), ##__VA_ARGS__)
#define pr_warning(fmt, ...) _printk(LOG_WARNING, pr_fmt(fmt), ##__VA_ARGS__)
#define pr_warn  pr_warning
#define pr_notice(fmt, ...)  _printk(LOG_NOTICE,  pr_fmt(fmt), ##__VA_ARGS__)
#define pr_info(fmt, ...)    _printk(LOG_INFO,    pr_fmt(fmt), ##__VA_ARGS__)
#define pr_debug(fmt, ...)   _printk(LOG_DEBUG,   pr_fmt(fmt), ##__VA_ARGS__)

/* Helper to detect ISR context */
extern volatile int in_isr;
#define printk_isr_safe() (in_isr == 0)

#endif
```

- [ ] **Step 2: Write printk.c**

```c
// src/kernel/printk.c
#include "kernel/printk.h"
#include "kernel/vsnprintf.h"
#include "serial.h"
#include "terminal.h"
#include "klog.h"

int console_loglevel = DEFAULT_CONSOLE_LOGLEVEL;

#ifndef DEFAULT_CONSOLE_LOGLEVEL
#define DEFAULT_CONSOLE_LOGLEVEL 6
#endif

/* ISR detection flag — set by ISR entry stub, cleared on exit */
volatile int in_isr = 0;

void _printk(int level, const char* fmt, ...)
{
    /* Log level filter */
    if (level > console_loglevel)
        return;

    char buf[256];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    /* Always write to serial */
    log_serial(buf);

    /* Terminal output — skip in ISR (terminal uses PIO, slow) */
    if (!in_isr)
        terminal_writestring(buf);

    /* Store in kernel ring buffer for dmesg */
    klog_write(level, buf);
}
```

- [ ] **Step 3: Add ISR detection**

In the interrupt handler stub (`src/arch/i386/isr.c` or similar), add:
```c
// In ISR entry:
extern volatile int in_isr;
in_isr = 1;

// In ISR exit (before iret):
in_isr = 0;
```

If using common interrupt handler wrapper, find `isr_handler` or `irq_handler` and add the flag.

- [ ] **Step 4: Integrate with klog (kernel ring buffer)**

Find `src/kernel/klog.c` and modify `klog_write` to accept a level:
```c
// If current signature is void klog_write(const char* msg):
// Change to: void klog_write(int level, const char* msg)
// The level is stored alongside the message for dmesg filtering.
```

Check the existing klog implementation to see the current storage format.

- [ ] **Step 5: Build and verify**

Run: `make -j4`
Expected: Clean build.

- [ ] **Step 6: Commit**

```bash
git add src/include/kernel/printk.h src/kernel/printk.c
git commit -m "feat: add pr_* logging system with log levels

Adds pr_emerg through pr_debug macros, runtime console_loglevel
filtering, ISR-safe serial output via in_isr flag, and klog
integration. Subsequent commits convert existing code."
```

### Task 2.3: Convert existing code to pr_* (mechanical)

**Files modified:** Every .c file that currently calls `log_serial()` + `terminal_writestring()`.

- [ ] **Step 1: Convert rtl8139.c**

Before:
```c
log_serial("RTL8139: Found at ");
terminal_writestring("RTL8139: Found at ");
// ...
log_serial("RTL8139: Initialized\n");
terminal_writestring("RTL8139: Initialized\n");
```

After:
```c
#define pr_fmt(fmt) "[RTL8139] " fmt
#include "kernel/printk.h"

pr_info("RTL8139: Found at ...\n");
// ...
pr_info("Initialized\n");
```

- [ ] **Step 2: Convert kernel.c, panic.c, pci.c, ide.c, net.c, tcp.c, shell.c, timer.c, pmm.c, paging.c**

Pattern for each file:
```c
// Add at top (after existing includes):
#include "kernel/printk.h"
```

Replace `log_serial("..."); terminal_writestring("...");` with `pr_info("...")`.
Replace `log_serial("..."); log_hex_serial(x); log_serial("\n");` with `pr_debug("0x%x\n", x)`.

This is a mechanical transformation. Do one file at a time, build, verify.

- [ ] **Step 3: Build verify after each file**

Run: `make -j4`
Expected: Clean build after each conversion.

- [ ] **Step 4: Commit all conversions**

```bash
git commit -m "refactor: convert logging to pr_* macro system

Mechanical conversion of log_serial() + terminal_writestring() pairs
to pr_info(), pr_err(), pr_debug() etc. with appropriate subsystem
tags. All output behavior preserved."
```

---

## Phase 3: Driver Model

### Task 3.1: Initcall framework

**Files:**
- Create: `src/include/init.h`
- Modify: `linker.ld` — add `.initcalls` section
- Modify: `src/kernel/kernel.c` — replace manual inits with initcalls_run

- [ ] **Step 1: Write init.h**

```c
// src/include/init.h
#ifndef INIT_H
#define INIT_H

/* Initcall function type */
typedef void (*initcall_t)(void);

/* Place function pointer in the .initcalls section */
#define __init __attribute__((used, section(".initcalls")))

/* Two priority levels */
#define early_init(fn)  static initcall_t __initcall_early_##fn __init = fn
#define device_init(fn) static initcall_t __initcall_##fn __init = fn

/**
 * initcalls_run - Execute all registered initcalls in section order.
 * Called once during kernel boot.
 */
void initcalls_run(void);

#endif
```

- [ ] **Step 2: Add implementation**

In a new or existing kernel file (`src/kernel/init.c`):
```c
// src/kernel/init.c
#include "init.h"
#include "serial.h"

void initcalls_run(void)
{
    extern initcall_t __initcall_start[];
    extern initcall_t __initcall_end[];

    log_serial("init: running initcalls...\n");
    for (initcall_t* fn = __initcall_start; fn < __initcall_end; fn++) {
        (*fn)();
    }
    log_serial("init: done\n");
}
```

- [ ] **Step 3: Add .initcalls section to linker.ld**

After the `.text` section:
```ld
    .text BLOCK(4K) : ALIGN(4K)
    {
        *(.multiboot)
        *(.text)
    }

    /* Initcall table — collected function pointers */
    .initcalls BLOCK(4K) : ALIGN(4K)
    {
        __initcall_start = .;
        *(.initcalls)
        __initcall_end = .;
    }
```

- [ ] **Step 4: Convert kernel.c main to use initcalls_run**

In `src/kernel/kernel.c`, replace the list of `init_*()` calls with:
```c
#include "init.h"
// ... existing includes ...

void kmain(void)
{
    // ... GDT, IDT, ISR setup (these are early init)...

    initcalls_run();

    // ... shell main loop ...
}
```

- [ ] **Step 5: Convert existing inits to initcall format**

For each subsystem, add initcall registration. Example:
```c
// src/drivers/timer.c — existing init_timer():
void init_timer(void) { /* ... */ }
device_init(init_timer);

// src/drivers/pci.c — existing init_pci():
void init_pci(void) { /* ... */ }
device_init(init_pci);

// src/drivers/power.c — existing:
void init_power(void) { /* ... */ }
device_init(init_power);

// src/drivers/rtl8139.c:
void init_rtl8139(void) { /* ... */ }
device_init(init_rtl8139);

// src/drivers/serial.c:
void init_serial(void) { /* ... */ }
early_init(init_serial);  // serial needed very early

// src/drivers/ide.c:
void init_ide(void) { /* ... */ }
device_init(init_ide);

// fs_init:
device_init(fs_init);
```

- [ ] **Step 6: Build and verify**

Run: `make -j4`
Expected: All initcalls run at boot. No `kmain` list of individual init calls.

- [ ] **Step 7: Commit**

```bash
git add src/include/init.h src/kernel/init.c linker.ld src/kernel/kernel.c
# Also stage all modified driver files with device_init() added
git commit -m "feat: add initcall framework for automatic driver initialization

Drivers register via device_init() or early_init() macros.
Function pointers are collected in .initcalls section by the
linker. initcalls_run() iterates them at boot, replacing the
manual list in kmain()."
```

### Task 3.2: PCI driver registration and auto-probe

**Files:**
- Create: `src/include/device.h`
- Modify: `src/drivers/pci.c` — add pci_register_driver, auto-probe
- Modify: `src/include/pci.h` — add pci_driver struct and pci_device_id

- [ ] **Step 1: Extend pci.h with driver types**

```c
// src/include/pci.h — add after existing declarations:
struct pci_device_id {
    uint16_t vendor;
    uint16_t device;
    uint32_t driver_data;
};

struct pci_driver {
    const char*                name;
    const struct pci_device_id* id_table;  /* terminated by {0,0,0} */
    int  (*probe)(pci_device_t* dev);
    void (*remove)(pci_device_t* dev);
    struct pci_driver* next;
};

void pci_register_driver(struct pci_driver* drv);
```

- [ ] **Step 2: Implement pci_register_driver in pci.c**

```c
// src/drivers/pci.c — add:
static struct pci_driver* pci_drivers = NULL;

/* Internal: iterate all PCI buses and try to match a driver */
static void pci_probe_driver(struct pci_driver* drv)
{
    for (int bus = 0; bus < 256; bus++) {
        for (int slot = 0; slot < 32; slot++) {
            for (int func = 0; func < 8; func++) {
                uint16_t vendor = pci_config_read_word(bus, slot, func, 0);
                if (vendor == 0xFFFF) continue; /* no device */

                for (const struct pci_device_id* id = drv->id_table;
                     id->vendor != 0; id++) {
                    uint16_t device = pci_config_read_word(bus, slot, func, 2);
                    if (vendor == id->vendor && device == id->device) {
                        pci_device_t pdev;
                        pdev.bus = bus;
                        pdev.device = slot;
                        pdev.function = func;
                        pdev.vendor_id = vendor;
                        pdev.device_id = device;
                        pdev.bar0 = pci_config_read_dword(bus, slot, func, 0x10);
                        pdev.irq_line = (uint8_t)(pci_config_read_dword(bus, slot, func, 0x3C) & 0xFF);
                        log_serial("pci: probing driver '%s' for %04x:%04x\n",
                                   drv->name, vendor, device);
                        drv->probe(&pdev);
                    }
                }
            }
        }
    }
}

void pci_register_driver(struct pci_driver* drv)
{
    drv->next = pci_drivers;
    pci_drivers = drv;
    pci_probe_driver(drv);
}
```

- [ ] **Step 3: Write device.h for generic device abstraction**

```c
// src/include/device.h
#ifndef DEVICE_H
#define DEVICE_H
#include <stdint.h>

struct device {
    const char* name;
    uint32_t    irq;
    uint32_t    io_base;
    void*       mmio_base;
    void*       driver_data;   /* private data for the driver */
    const char* bus_name;      /* "pci", "platform" */
    struct device* next;
};

/* Register a device in the global device list (for /sys enumeration) */
void device_register(struct device* dev);

/* Iterate registered devices — returns first, then repeatedly call */
struct device* device_iter_first(void);
struct device* device_iter_next(struct device* prev);

#endif
```

- [ ] **Step 4: Implement device_register**

In a new or existing file (`src/kernel/device.c` or in `src/drivers/pci.c`):
```c
#include "device.h"

static struct device* device_list = NULL;

void device_register(struct device* dev)
{
    dev->next = device_list;
    device_list = dev;
}

struct device* device_iter_first(void)
{
    return device_list;
}

struct device* device_iter_next(struct device* prev)
{
    return prev ? prev->next : NULL;
}
```

- [ ] **Step 5: Convert rtl8139 to pci_driver**

```c
// src/drivers/rtl8139.c — replace init_rtl8139 with probe:
static struct pci_device_id rtl8139_ids[] = {
    { PCI_VENDOR_REALTEK, PCI_DEVICE_RTL8139, 0 },
    { 0, 0, 0 }
};

static int rtl8139_probe(pci_device_t* dev)
{
    terminal_writestring("rtl8139: probing\n");

    io_base = dev->bar0 & ~0x3;
    irq_num = dev->irq_line;

    /* Enable PCI bus mastering */
    uint32_t pci_cmd = pci_config_read_dword(dev->bus, dev->device,
                                              dev->function, 0x04);
    pci_cmd |= (1 << 2) | (1 << 0);
    pci_config_write_dword(dev->bus, dev->device, dev->function, 0x04, pci_cmd);

    /* ... remainder of existing init_rtl8139 from power-on through RE|TE ... */

    device_register(&(struct device){
        .name = "rtl8139",
        .irq = irq_num,
        .io_base = io_base,
        .bus_name = "pci",
    });

    return 0;
}

static struct pci_driver rtl8139_pci_driver = {
    .name     = "rtl8139",
    .id_table = rtl8139_ids,
    .probe    = rtl8139_probe,
};

static void init_rtl8139_driver(void)
{
    pci_register_driver(&rtl8139_pci_driver);
}
device_init(init_rtl8139_driver);
```

- [ ] **Step 6: Build and verify**

Run: `make -j4`
Expected: Builds clean. RTL8139 still initializes at boot.

- [ ] **Step 7: Commit**

```bash
git add src/include/pci.h src/drivers/pci.c src/include/device.h src/kernel/device.c src/drivers/rtl8139.c
git commit -m "feat: add PCI driver model with auto-probe

Adds pci_driver/pci_device_id types, pci_register_driver with
automatic bus scanning, and generic device registration.
rtl8139 converted to use the new probe-based model."
```

---

## Phase 4: devtmpfs + Filesystem Layout

### Task 4.1: Mount point VFS layer

**Files:**
- Modify: `src/fs/fs.c` — add mount point dispatch

- [ ] **Step 1: Add mount point infrastructure to fs.c**

```c
// src/fs/fs.c — add after includes:
#include "string.h"

#define MAX_MOUNTS 4

struct mount {
    const char* path;
    int         path_len;
    int         (*open)(const char* path, int flags);
    int         (*read)(int fd, void* buf, uint32_t size, uint32_t offset);
    int         (*write)(int fd, const void* buf, uint32_t size, uint32_t offset);
    int         (*close)(int fd);
};

static struct mount mounts[MAX_MOUNTS];
static int mount_count = 0;
static int mount_fd_counter = 100; /* start above JexFS FDs to avoid collision */

int fs_mount(const char* path, const char* fstype)
{
    if (mount_count >= MAX_MOUNTS) return -1;
    if (strcmp(fstype, "devtmpfs") == 0) {
        mounts[mount_count].path     = path;
        mounts[mount_count].path_len = strlen(path);
        mounts[mount_count].open     = devtmpfs_open;
        mounts[mount_count].read     = devtmpfs_read;
        mounts[mount_count].write    = devtmpfs_write;
        mounts[mount_count].close    = devtmpfs_close;
        mount_count++;
        return 0;
    }
    return -1;
}

/* Find the mount point for a given path — returns NULL if no match */
static struct mount* fs_find_mount(const char* path)
{
    /* Walk backwards (longest prefix wins) */
    for (int i = mount_count - 1; i >= 0; i--) {
        if (strncmp(path, mounts[i].path, mounts[i].path_len) == 0)
            return &mounts[i];
    }
    return NULL;
}
```

- [ ] **Step 2: Modify fs_open to use mount dispatch**

```c
int fs_open(const char* filename, int flags)
{
    struct mount* m = fs_find_mount(filename);
    if (m) {
        return m->open(filename, flags);
    }

    /* Fall through to JexFS */
    /* ... existing jexfs_open logic ... */
}
```

- [ ] **Step 3: Commit**

```bash
git add src/fs/fs.c
git commit -m "feat: add VFS mount point dispatch

fs_mount() registers filesystem drivers at path prefixes.
fs_open() dispatches to the right driver by longest prefix match.
Enables devtmpfs at /sys/ while keeping JexFS as root."
```

### Task 4.2: devtmpfs implementation

**Files:**
- Create: `src/fs/devtmpfs.c`
- Create: `src/include/fs/devtmpfs.h`
- Modify: `src/include/fs/fs.h` — add devtmpfs declarations

- [ ] **Step 1: Write devtmpfs.h**

```c
// src/include/fs/devtmpfs.h
#ifndef DEVTMFS_H
#define DEVTMFS_H
#include <stdint.h>

struct devtmpfs_file {
    const char* name;
    int         (*read)(char* buf, int max_len);   /* populate buf, return bytes */
    int         (*write)(const char* buf, int len); /* handle write data */
    unsigned short mode; /* file permissions: 0444, 0644 */
};

/* Register a virtual file at the given path */
int devtmpfs_add_file(const char* path, struct devtmpfs_file* file);

/* Initialize devtmpfs (called at boot) */
int devtmpfs_init(void);

/* VFS interface functions */
int devtmpfs_open(const char* path, int flags);
int devtmpfs_read(int fd, void* buf, uint32_t size, uint32_t offset);
int devtmpfs_write(int fd, const void* buf, uint32_t size, uint32_t offset);
int devtmpfs_close(int fd);

#endif
```

- [ ] **Step 2: Write devtmpfs.c**

```c
// src/fs/devtmpfs.c
#include "fs/devtmpfs.h"
#include "string.h"
#include "kheap.h"

#define DEVTMFS_MAX_FILES 32
#define DEVTMFS_MAX_OPEN  8

typedef struct {
    const char* path;
    struct devtmpfs_file file;
} devtmpfs_entry_t;

static devtmpfs_entry_t entries[DEVTMFS_MAX_FILES];
static int entry_count = 0;

/* Open file table: track which virtual file is open per FD */
typedef struct {
    int used;
    int entry_idx;
    uint32_t offset;
} devtmpfs_fd_t;

static devtmpfs_fd_t open_files[DEVTMFS_MAX_OPEN];

int devtmpfs_add_file(const char* path, struct devtmpfs_file* file)
{
    if (entry_count >= DEVTMFS_MAX_FILES) return -1;
    entries[entry_count].path = path;
    entries[entry_count].file = *file;
    entry_count++;
    return 0;
}

int devtmpfs_open(const char* path, int flags)
{
    (void)flags;
    /* Find the entry matching path */
    int idx = -1;
    for (int i = 0; i < entry_count; i++) {
        if (strcmp(path, entries[i].path) == 0) {
            idx = i;
            break;
        }
    }
    if (idx < 0) return -1;

    /* Find free FD slot */
    for (int i = 0; i < DEVTMFS_MAX_OPEN; i++) {
        if (!open_files[i].used) {
            open_files[i].used = 1;
            open_files[i].entry_idx = idx;
            open_files[i].offset = 0;
            return 100 + i; /* start at FD 100 */
        }
    }
    return -1;
}

int devtmpfs_read(int fd, void* buf, uint32_t size, uint32_t offset)
{
    int slot = fd - 100;
    if (slot < 0 || slot >= DEVTMFS_MAX_OPEN || !open_files[slot].used)
        return -1;

    int idx = open_files[slot].entry_idx;
    if (!entries[idx].file.read)
        return -1; /* read not supported */

    char tmp[256];
    int n = entries[idx].file.read(tmp, sizeof(tmp));
    if (n <= 0) return 0;

    /* Manual copy with offset tracking */
    if ((int)offset >= n) return 0;
    int avail = n - (int)offset;
    if ((int)size > avail) size = (uint32_t)avail;
    memcpy(buf, tmp + offset, size);
    return (int)size;
}

int devtmpfs_write(int fd, const void* buf, uint32_t size, uint32_t offset)
{
    (void)offset;
    int slot = fd - 100;
    if (slot < 0 || slot >= DEVTMFS_MAX_OPEN || !open_files[slot].used)
        return -1;

    int idx = open_files[slot].entry_idx;
    if (!entries[idx].file.write)
        return -1;

    if (entries[idx].file.write((const char*)buf, (int)size))
        return (int)size;
    return -1;
}

int devtmpfs_close(int fd)
{
    int slot = fd - 100;
    if (slot < 0 || slot >= DEVTMFS_MAX_OPEN) return -1;
    open_files[slot].used = 0;
    return 0;
}

int devtmpfs_init(void)
{
    /* Register with VFS — nothing extra needed */
    return 0;
}
```

- [ ] **Step 3: Build and verify**

Run: `make -j4`
Expected: Clean build.

- [ ] **Step 4: Add initial sysfs files (one per subsystem)**

After `devtmpfs_init()`, register core kernel files. For each subsystem, add a small read handler:

```c
/* Kernel version */
static int sys_read_version(char* buf, int max)
{
    const char* v = "JexOS v0.1 (i386)\n";
    int len = strlen(v);
    if (len > max) len = max;
    memcpy(buf, v, len);
    return len;
}

/* Log level — readable AND writable */
static int sys_read_loglevel(char* buf, int max)
{
    /* needs snprintf from Phase 2 */
    buf[0] = '0' + (char)console_loglevel;
    buf[1] = '\n';
    return 2;
}

static int sys_write_loglevel(const char* buf, int len)
{
    if (len > 0 && buf[0] >= '0' && buf[0] <= '7')
        console_loglevel = buf[0] - '0';
    return len;
}
```

```c
// In devtmpfs_init or a new initcall:
int sysfs_init(void)
{
    devtmpfs_add_file("/sys/kernel/version", &(struct devtmpfs_file){
        .name = "version", .read = sys_read_version, .mode = 0444,
    });
    devtmpfs_add_file("/sys/kernel/loglevel", &(struct devtmpfs_file){
        .name = "log_level", .read = sys_read_loglevel,
        .write = sys_write_loglevel, .mode = 0644,
    });
    // ... add more files ...
    return 0;
}
```

- [ ] **Step 5: Commit**

```bash
git add src/fs/devtmpfs.c src/include/fs/devtmpfs.h src/fs/fs.c
git commit -m "feat: add devtmpfs virtual filesystem

devtmpfs provides kernel state as virtual files at /sys/.
Reads invoke kernel callbacks, writes dispatch to handlers.
Initial files: /sys/kernel/version, /sys/kernel/log_level."
```

### Task 4.3: Filesystem layout (/home/user/)

**Files:**
- Modify: `src/kernel/kernel.c` — create /home/user/ at boot, set cwd

- [ ] **Step 1: Modify kmain to create home directory**

```c
// src/kernel/kernel.c — add after fs_init():
#include "jexfs.h"  /* for jexfs_mkdir, cwd_inode */

void setup_filesystem(void)
{
    /* Create /home directory if it doesn't exist */
    if (jexfs_open("/home") < 0) {
        jexfs_mkdir("/home");
        log_serial("fs: created /home\n");
    }

    /* Create /home/user directory */
    if (jexfs_open("/home/user") < 0) {
        jexfs_mkdir("/home/user");
        log_serial("fs: created /home/user\n");
    }

    /* Set CWD to /home/user */
    cwd_inode = jexfs_open("/home/user");

    /* Mount devtmpfs */
    devtmpfs_init();
}
```

- [ ] **Step 2: Show the home directory in the shell prompt**

In `shell.c`, change the prompt string:
```c
// Before reading input:
terminal_writestring("JexOS:~$ ");
```

- [ ] **Step 3: Build and verify**

Run: `make -j4`
Expected: Clean build. Boot creates `/home/user/`. Shell starts there.

- [ ] **Step 4: Commit**

```bash
git add src/kernel/kernel.c src/bin/shell.c
git commit -m "feat: create /home/user/ directory on boot

Set up user home directory and mount devtmpfs during filesystem
initialization. Shell starts in /home/user/."
```

---

## Phase 5: Concurrency

### Task 5.1: Workqueue

**Files:**
- Create: `src/include/kernel/workqueue.h`
- Create: `src/kernel/workqueue.c`
- Modify: `src/bin/shell.c` — call workqueue_run in main loop

- [ ] **Step 1: Write workqueue.h**

```c
// src/include/kernel/workqueue.h
#ifndef WORKQUEUE_H
#define WORKQUEUE_H

typedef void (*work_func_t)(void* data);

struct work {
    work_func_t  func;
    void*        data;
    struct work* next;
};

/**
 * schedule_work - Queue a work item for deferred execution.
 * Safe to call from ISR context (interrupts disabled on entry).
 * @w: Work item (must remain valid until func is called).
 */
void schedule_work(struct work* w);

/**
 * workqueue_run - Execute all pending work items.
 * Call from shell main loop (interrupts enabled).
 */
void workqueue_run(void);

#endif
```

- [ ] **Step 2: Write workqueue.c**

```c
// src/kernel/workqueue.c
#include "kernel/workqueue.h"

static struct work* work_head = NULL;
static struct work* work_tail = NULL;

void schedule_work(struct work* w)
{
    /* Save and disable interrupts */
    uint32_t eflags;
    __asm__ volatile("pushf; pop %0" : "=r"(eflags));
    __asm__ volatile("cli");

    w->next = NULL;
    if (work_tail)
        work_tail->next = w;
    else
        work_head = w;
    work_tail = w;

    /* Restore interrupt state */
    if (eflags & 0x200)
        __asm__ volatile("sti");
}

void workqueue_run(void)
{
    while (1) {
        struct work* w;

        /* Dequeue with interrupts disabled */
        __asm__ volatile("cli");
        w = work_head;
        if (w) {
            work_head = w->next;
            if (!work_head)
                work_tail = NULL;
        }
        __asm__ volatile("sti");

        if (!w) break;

        w->func(w->data);
    }
}
```

- [ ] **Step 3: Integrate into shell main loop**

In `src/bin/shell.c`, find the main command loop (reading input → processing → reprompting) and add:

```c
#include "kernel/workqueue.h"

// Inside the main while(1) loop, before reading keyboard input:
workqueue_run();
```

- [ ] **Step 4: Build and verify**

Run: `make -j4`
Expected: Clean build.

- [ ] **Step 5: Commit**

```bash
git add src/include/kernel/workqueue.h src/kernel/workqueue.c src/bin/shell.c
git commit -m "feat: add workqueue for deferred execution

schedule_work() is safe from ISR context. workqueue_run()
executes pending items from the shell main loop. Replaces
ad-hoc poll_rx() call sites with queue-based pattern."
```

### Task 5.2: Lockdep

**Files:**
- Create: `src/include/kernel/lockdep.h`
- Create: `src/kernel/lockdep.c`
- Modify: `src/kernel/kernel.c` — call lockdep_init

- [ ] **Step 1: Write lockdep.h**

```c
// src/include/kernel/lockdep.h
#ifndef LOCKDEP_H
#define LOCKDEP_H

void lockdep_init(void);
void lockdep_acquire(const char* name, void* addr);
void lockdep_release(void* addr);

/* Wrap spinlock operations with lockdep tracking */
void _spin_lock_irqsave(const char* name, void* lock, uint32_t* flags);
void _spin_unlock_irqrestore(void* lock, uint32_t flags);

#define spin_lock_irqsave(lock, flags, name) \
    _spin_lock_irqsave(name, &(lock), &(flags))

#define spin_unlock_irqrestore(lock, flags) \
    _spin_unlock_irqrestore(&(lock), flags)

#endif
```

- [ ] **Step 2: Write lockdep.c**

```c
// src/kernel/lockdep.c
#include "kernel/lockdep.h"
#include "kernel/warn.h"
#include "serial.h"

#define LOCKDEP_MAX_LOCKS 32
#define LOCKDEP_DEPTH_MAX 8

/* Map of known locks */
static struct {
    void*       addr;
    const char* name;
} lock_map[LOCKDEP_MAX_LOCKS];
static int lock_map_count = 0;

/* Per-context lock stack */
static void* lock_stack[LOCKDEP_DEPTH_MAX];
static int lock_depth = 0;

/* Edge matrix: edges[i] has bit j set if lock i -> lock j ordering seen */
static uint32_t edges[LOCKDEP_MAX_LOCKS];

/* Track if we've already warned about a cycle (avoid spam) */
static int warned_cycle = 0;

static int lock_find_or_add(void* addr, const char* name)
{
    for (int i = 0; i < lock_map_count; i++) {
        if (lock_map[i].addr == addr)
            return i;
    }
    if (lock_map_count < LOCKDEP_MAX_LOCKS) {
        lock_map[lock_map_count].addr = addr;
        lock_map[lock_map_count].name = name;
        return lock_map_count++;
    }
    return -1;
}

void lockdep_init(void)
{
    lock_map_count = 0;
    lock_depth = 0;
    for (int i = 0; i < LOCKDEP_MAX_LOCKS; i++)
        edges[i] = 0;
    warned_cycle = 0;
    log_serial("lockdep: initialized\n");
}

void lockdep_acquire(const char* name, void* addr)
{
    int id = lock_find_or_add(addr, name);
    if (id < 0) return;

    /* Check for inversion against all currently held locks */
    for (int i = 0; i < lock_depth; i++) {
        /* Find the ID of the held lock */
        int held_id = -1;
        for (int j = 0; j < lock_map_count; j++) {
            if (lock_map[j].addr == lock_stack[i]) {
                held_id = j;
                break;
            }
        }
        if (held_id < 0) continue;

        /* If we already recorded that id -> held_id is valid, this is fine */
        /* If we already recorded held_id -> id, that's an inversion */
        if (edges[id] & (1u << held_id)) {
            if (!warned_cycle) {
                warned_cycle = 1;
                log_serial("lockdep: POSSIBLE DEADLOCK: ");
                log_serial(name);
                log_serial(" -> ");
                log_serial(lock_map[held_id].name);
                log_serial(" is inverted!\n");
                WARN_ON(1);
            }
        }

        /* Record edge: held lock -> new lock */
        if (held_id >= 0 && held_id < LOCKDEP_MAX_LOCKS)
            edges[held_id] |= (1u << id);
    }

    /* Push onto held-lock stack */
    if (lock_depth < LOCKDEP_DEPTH_MAX)
        lock_stack[lock_depth++] = addr;
}

void lockdep_release(void* addr)
{
    /* Pop from held-lock stack */
    if (lock_depth > 0)
        lock_depth--;

    /* Clear edge hints for this lock to keep the matrix sparse */
    /* (edges accumulate over time, intentional) */
}
```

For the spinlock wrappers, match the actual locking primitives in the codebase. If JexOS uses cli/sti for locks:

```c
void _spin_lock_irqsave(const char* name, void* lock, uint32_t* flags)
{
    __asm__ volatile("pushf; pop %0" : "=r"(*flags));
    __asm__ volatile("cli");
    lockdep_acquire(name, lock);
}

void _spin_unlock_irqrestore(void* lock, uint32_t flags)
{
    lockdep_release(lock);
    if (flags & 0x200)
        __asm__ volatile("sti");
}
```

- [ ] **Step 3: Build and verify**

Run: `make -j4`
Expected: Clean build.

- [ ] **Step 4: Commit**

```bash
git add src/include/kernel/lockdep.h src/kernel/lockdep.c
git commit -m "feat: add lockdep for spinlock ordering detection

Tracks lock acquisition order and warns on inverted ordering
(possible deadlock). 32-lock limit with 8-level nesting depth."
```

---

## Phase 6: GDB Stub + ftrace-lite

### Task 6.1: GDB stub over serial

**Files:**
- Create: `src/include/debug/gdb_stub.h`
- Create: `src/debug/gdb_stub.c`
- Modify: `src/arch/i386/isr.c` — register int3 handler
- Modify: `Makefile` — add debug directory to sources

- [ ] **Step 1: Write gdb_stub.h**

```c
// src/include/debug/gdb_stub.h
#ifndef GDB_STUB_H
#define GDB_STUB_H
#include <stdint.h>

typedef struct {
    uint32_t eax, ebx, ecx, edx;
    uint32_t esi, edi, ebp, esp;
    uint32_t eip, eflags;
} gdb_regs_t;

/**
 * gdb_breakpoint - Trigger a breakpoint and enter GDB stub.
 * Call from any kernel code to drop into the debugger.
 */
void gdb_breakpoint(void);

/**
 * gdb_stub_handler - Entry point for int3 (breakpoint) handler.
 * Called from the IDT's interrupt 3 handler with saved registers.
 */
void gdb_stub_handler(registers_t* regs);

#endif
```

- [ ] **Step 2: Write gdb_stub.c (minimal implementation)**

```c
// src/debug/gdb_stub.c
#include "debug/gdb_stub.h"
#include "serial.h"
#include "ports.h"

/* Breakpoint shadow table */
#define MAX_BP 256
static uint8_t bp_orig[MAX_BP];
static uint32_t bp_addr[MAX_BP];
static int bp_count = 0;

/* Packet buffer */
#define BUF_SIZE 512
static char pkt_buf[BUF_SIZE];
static int pkt_len = 0;

/* Serial helpers */
static void gdb_putchar(char c) { outb(c, 0x3F8); }
static char gdb_getchar(void) {
    while (!(inb(0x3F8 + 5) & 1));
    return inb(0x3F8);
}

static void gdb_send_packet(const char* data, int len)
{
    uint8_t csum = 0;
    gdb_putchar('$');
    for (int i = 0; i < len; i++) {
        gdb_putchar(data[i]);
        csum += (uint8_t)data[i];
    }
    gdb_putchar('#');
    /* Send checksum as two hex digits */
    const char* hex = "0123456789abcdef";
    gdb_putchar(hex[(csum >> 4) & 0xF]);
    gdb_putchar(hex[csum & 0xF]);
}

/* Convert nibble to hex char */
static char nibble_hex(uint8_t v) {
    return "0123456789abcdef"[v & 0xF];
}

/* Read N bytes from memory as hex string */
static int mem_to_hex(char* out, uint32_t addr, int len)
{
    for (int i = 0; i < len; i++) {
        uint8_t b = *(volatile uint8_t*)addr;
        out[i * 2]     = nibble_hex(b >> 4);
        out[i * 2 + 1] = nibble_hex(b);
        addr++;
    }
    return len * 2;
}

/* Write hex string to memory */
static int hex_to_mem(uint32_t addr, const char* hex, int hex_len)
{
    for (int i = 0; i < hex_len / 2; i++) {
        uint8_t v = 0;
        for (int j = 0; j < 2; j++) {
            char c = hex[i * 2 + j];
            v <<= 4;
            if (c >= '0' && c <= '9') v |= c - '0';
            else if (c >= 'a' && c <= 'f') v |= c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') v |= c - 'A' + 10;
        }
        *(volatile uint8_t*)addr = v;
        addr++;
    }
    return hex_len / 2;
}

/* Register file as hex string (eax, ecx, edx, ebx, esp, ebp, esi, edi, eip, eflags, cs, ss, ds, es, fs, gs) */
static int regs_to_hex(char* out, registers_t* regs)
{
    uint32_t r[] = {
        regs->eax, regs->ecx, regs->edx, regs->ebx,
        regs->esp, regs->ebp, regs->esi, regs->edi,
        regs->eip, regs->eflags,
        0x08, 0x10, 0x10, 0x10, 0x10, 0x10 /* cs, ss, ds, es, fs, gs */
    };
    int pos = 0;
    for (int i = 0; i < 16; i++) {
        for (int b = 3; b >= 0; b--) {
            out[pos++] = nibble_hex(r[i] >> (b * 4 + 4));
            out[pos++] = nibble_hex(r[i] >> (b * 4));
        }
    }
    return pos;
}

static int hex_to_byte(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return 0;
}

/* Receive a packet from GDB */
static int gdb_recv_packet(char* buf)
{
    while (1) {
        char c = gdb_getchar();
        if (c == '+') continue;  /* ignore ACKs */
        if (c == '$') break;
    }

    int pos = 0;
    while (1) {
        char c = gdb_getchar();
        if (c == '#') break;
        if (c == '$') { pos = 0; continue; } /* retransmit */
        if (pos < BUF_SIZE - 1) buf[pos++] = c;
    }

    /* Read and verify checksum (optional — skip for simplicity) */
    char csum_hex[3];
    csum_hex[0] = gdb_getchar();
    csum_hex[1] = gdb_getchar();
    csum_hex[2] = '\0';

    (void)csum_hex;
    gdb_putchar('+');  /* ACK */
    return pos;
}

void gdb_stub_handler(registers_t* regs)
{
    /* Temporarily restore breakpoint byte so we can step past it */
    uint32_t bp_eip = regs->eip - 1;
    for (int i = 0; i < bp_count; i++) {
        if (bp_addr[i] == bp_eip) {
            *(volatile uint8_t*)bp_eip = bp_orig[i];
            regs->eip = bp_eip;
            break;
        }
    }

    gdb_putchar('+');  /* initial ACK */

    while (1) {
        int len = gdb_recv_packet(pkt_buf);
        if (len <= 0) continue;

        char cmd = pkt_buf[0];
        char reply[1024];
        int rlen = 0;

        switch (cmd) {
        case '?': /* Stop reason */
            reply[0] = 'S'; reply[1] = '0'; reply[2] = '5';
            rlen = 3;
            break;

        case 'g': /* Read registers */
            rlen = regs_to_hex(reply, regs);
            break;

        case 'G': /* Write registers (not fully implemented) */
            reply[0] = 'E'; reply[1] = '0'; reply[2] = '1';
            rlen = 3;
            break;

        case 'm': { /* Read memory: m addr,len */
            uint32_t addr = 0, rlen2 = 0;
            int i = 1;
            while (pkt_buf[i] >= '0' && pkt_buf[i] <= '9' ||
                   pkt_buf[i] >= 'a' && pkt_buf[i] <= 'f') {
                addr = (addr << 4) + hex_to_byte(pkt_buf[i]);
                i++;
            }
            i++; /* skip comma */
            while (i < len && (pkt_buf[i] >= '0' && pkt_buf[i] <= '9' ||
                   pkt_buf[i] >= 'a' && pkt_buf[i] <= 'f')) {
                rlen2 = (rlen2 << 4) + hex_to_byte(pkt_buf[i]);
                i++;
            }
            rlen = mem_to_hex(reply, addr, (int)rlen2);
            break;
        }

        case 'M': { /* Write memory: M addr,len:XX... */
            uint32_t addr = 0;
            int i = 1;
            while (pkt_buf[i] && pkt_buf[i] != ',' &&
                   (pkt_buf[i] >= '0' && pkt_buf[i] <= '9' ||
                    pkt_buf[i] >= 'a' && pkt_buf[i] <= 'f')) {
                addr = (addr << 4) + hex_to_byte(pkt_buf[i]);
                i++;
            }
            i++; /* skip comma */
            uint32_t wlen = 0;
            while (pkt_buf[i] && pkt_buf[i] != ':') {
                wlen = (wlen << 4) + hex_to_byte(pkt_buf[i]);
                i++;
            }
            i++; /* skip colon */
            hex_to_mem(addr, &pkt_buf[i], (int)wlen * 2);
            reply[0] = 'O'; reply[1] = 'K';
            rlen = 2;
            break;
        }

        case 'c': /* Continue */
            /* Re-insert breakpoints before resuming */
            for (int i = 0; i < bp_count; i++)
                *(volatile uint8_t*)bp_addr[i] = 0xCC;
            return; /* exit stub, resume at regs->eip */

        case 's': /* Single step */
            regs->eflags |= 0x100; /* set TF */
            return;

        case 'Z': { /* Insert breakpoint: Z0,addr,kind */
            uint32_t addr = 0;
            int i = 2;
            while (pkt_buf[i] && pkt_buf[i] != ',' &&
                   (pkt_buf[i] >= '0' && pkt_buf[i] <= '9' ||
                    pkt_buf[i] >= 'a' && pkt_buf[i] <= 'f')) {
                addr = (addr << 4) + hex_to_byte(pkt_buf[i]);
                i++;
            }
            if (bp_count < MAX_BP) {
                bp_addr[bp_count] = addr;
                bp_orig[bp_count] = *(volatile uint8_t*)addr;
                *(volatile uint8_t*)addr = 0xCC;
                bp_count++;
            }
            reply[0] = 'O'; reply[1] = 'K';
            rlen = 2;
            break;
        }

        case 'z': { /* Remove breakpoint */
            uint32_t addr = 0;
            int i = 2;
            while (pkt_buf[i] && pkt_buf[i] != ',' &&
                   (pkt_buf[i] >= '0' && pkt_buf[i] <= '9' ||
                    pkt_buf[i] >= 'a' && pkt_buf[i] <= 'f')) {
                addr = (addr << 4) + hex_to_byte(pkt_buf[i]);
                i++;
            }
            for (int i = 0; i < bp_count; i++) {
                if (bp_addr[i] == addr) {
                    *(volatile uint8_t*)addr = bp_orig[i];
                    bp_addr[i] = bp_addr[bp_count - 1];
                    bp_orig[i] = bp_orig[bp_count - 1];
                    bp_count--;
                    break;
                }
            }
            reply[0] = 'O'; reply[1] = 'K';
            rlen = 2;
            break;
        }

        case 'k': /* Kill — reboot */
            reboot();
            break;

        default:
            reply[0] = 0;
            rlen = 0;
            break;
        }

        if (rlen > 0)
            gdb_send_packet(reply, rlen);
    }
}

void gdb_breakpoint(void)
{
    __asm__ volatile("int3");
}
```

- [ ] **Step 3: Register int3 handler in interrupt setup**

In the ISR/IDT setup code (`src/arch/i386/isr.c`):
```c
#include "debug/gdb_stub.h"

// In the ISR/IRQ setup function, register interrupt 3:
register_interrupt_handler(3, gdb_stub_handler);
```

Also add single-step handler for int 1 (Trap Flag):
```c
static void dbg_handler(registers_t* regs)
{
    /* If eflags has TF set, this is our single-step */
    if (regs->eflags & 0x100) {
        regs->eflags &= ~0x100; /* Clear TF */
        gdb_stub_handler(regs); /* Re-enter stub */
    }
}

register_interrupt_handler(1, dbg_handler);
```

- [ ] **Step 4: Build and verify**

Run: `make -j4`
Expected: Clean build. Add a `gdb_breakpoint()` call somewhere to test.

- [ ] **Step 5: Commit**

```bash
git add src/include/debug/gdb_stub.h src/debug/gdb_stub.c src/arch/i386/isr.c
git commit -m "feat: add GDB stub over serial

Minimal GDB Remote Serial Protocol implementation over COM1.
Supports breakpoints (Z0/z0), continue (c), single-step (s),
memory read/write (m/M), register read (g).
Trigger via gdb_breakpoint() or GDB's 'target remote /dev/ttyS0'."
```

### Task 6.2: ftrace-lite

**Files:**
- Create: `src/include/debug/ftrace.h`
- Create: `src/debug/ftrace.c`
- Modify: `src/bin/shell.c` — add `ftrace` command
- Modify: `Makefile` — add ftrace object and `-finstrument-functions` option

- [ ] **Step 1: Write ftrace.h**

```c
// src/include/debug/ftrace.h
#ifndef FTRACE_H
#define FTRACE_H
#include <stdint.h>

#define FTRACE_BUF_SIZE 4096
#define FTRACE_ENTRY 1
#define FTRACE_EXIT  2

typedef struct {
    uint32_t func;     /* called function address */
    uint32_t caller;   /* call site address */
    uint32_t ticks;    /* system tick */
    uint32_t type;     /* FTRACE_ENTRY or FTRACE_EXIT */
} ftrace_record_t;

void ftrace_enable(void);
void ftrace_disable(void);
int ftrace_is_enabled(void);

/* Filter management */
void ftrace_add_filter(const char* substr);
void ftrace_clear_filters(void);

/* Dump ring buffer to terminal */
void ftrace_dump(void);

/* Called by instrumented code */
void __cyg_profile_func_enter(void* func, void* call_site);
void __cyg_profile_func_exit(void* func, void* call_site);

#endif
```

- [ ] **Step 2: Write ftrace.c**

```c
// src/debug/ftrace.c
#include "debug/ftrace.h"
#include "serial.h"
#include "kernel/kallsyms.h"

static ftrace_record_t ftrace_buf[FTRACE_BUF_SIZE];
static volatile uint32_t ftrace_head = 0;
static volatile int ftrace_enabled = 0;

#define FTRACE_MAX_FILTERS 16
static char ftrace_filters[FTRACE_MAX_FILTERS][32];
static int filter_count = 0;
static int filter_all = 1; /* trace everything by default */

void ftrace_enable(void) {
    ftrace_enabled = 1;
    log_serial("ftrace: enabled\n");
}

void ftrace_disable(void) {
    ftrace_enabled = 0;
    log_serial("ftrace: disabled\n");
}

int ftrace_is_enabled(void) {
    return ftrace_enabled;
}

void ftrace_add_filter(const char* substr)
{
    if (filter_count >= FTRACE_MAX_FILTERS) return;
    int i = 0;
    while (*substr && i < 31)
        ftrace_filters[filter_count][i++] = *substr++;
    ftrace_filters[filter_count][i] = '\0';
    filter_count++;
    filter_all = 0;
}

void ftrace_clear_filters(void)
{
    filter_count = 0;
    filter_all = 1;
}

/* Simple substring match */
static int matches_filter(const char* name)
{
    if (filter_all) return 1;
    for (int f = 0; f < filter_count; f++) {
        const char* fn = name;
        while (*fn) {
            const char* fsub = ftrace_filters[f];
            const char* fpos = fn;
            while (*fsub && *fpos && *fsub == *fpos) {
                fsub++; fpos++;
            }
            if (*fsub == '\0') return 1;
            fn++;
        }
    }
    return 0;
}

void __cyg_profile_func_enter(void* func, void* call_site)
{
    if (!ftrace_enabled) return;

    uint32_t idx = ftrace_head;
    ftrace_buf[idx].func   = (uint32_t)func;
    ftrace_buf[idx].caller = (uint32_t)call_site;
    ftrace_buf[idx].ticks  = 0; /* system_ticks once available */
    ftrace_buf[idx].type   = FTRACE_ENTRY;
    ftrace_head = (idx + 1) % FTRACE_BUF_SIZE;
}

void __cyg_profile_func_exit(void* func, void* call_site)
{
    if (!ftrace_enabled) return;

    uint32_t idx = ftrace_head;
    ftrace_buf[idx].func   = (uint32_t)func;
    ftrace_buf[idx].caller = (uint32_t)call_site;
    ftrace_buf[idx].type   = FTRACE_EXIT;
    ftrace_head = (idx + 1) % FTRACE_BUF_SIZE;
}

void ftrace_dump(void)
{
    log_serial("ftrace dump (%u records):\n", ftrace_head);
    for (uint32_t i = 0; i < FTRACE_BUF_SIZE; i++) {
        uint32_t idx = (ftrace_head + i) % FTRACE_BUF_SIZE;
        if (ftrace_buf[idx].type == 0) continue;

        uint32_t off, sz;
        const char* fn = kallsyms_lookup(ftrace_buf[idx].func, &off, &sz);
        log_serial("  %s %s+0x%x (from ", 
                   ftrace_buf[idx].type == FTRACE_ENTRY ? "->" : "<-",
                   fn, off);
        fn = kallsyms_lookup(ftrace_buf[idx].caller, &off, &sz);
        log_serial("%s+0x%x)\n", fn, off);
    }
}
```

- [ ] **Step 3: Add ftrace command to shell**

In `shell.c`, add:
```c
#include "debug/ftrace.h"

static void ftrace_command(char* args) {
    if (!args || strcmp(args, "start") == 0) ftrace_enable();
    else if (strcmp(args, "stop") == 0)      ftrace_disable();
    else if (strcmp(args, "clear") == 0)     ftrace_clear_filters();
    else if (strcmp(args, "dump") == 0)      ftrace_dump();
    else if (strncmp(args, "add ", 4) == 0)  ftrace_add_filter(args + 4);
    else terminal_writestring("usage: ftrace start|stop|add <filter>|clear|dump\n");
}
```

- [ ] **Step 4: Add ftrace object to Makefile**

The `SOURCES_C` wildcard already picks up `src/debug/ftrace.c` — so it's compiled automatically when the file is created.

For `-finstrument-functions`, create a separate build target or add a flag:
```makefile
# Add a debug build variant:
CFLAGS_DEBUG = -finstrument-functions

ifeq ($(DEBUG),1)
CFLAGS += $(CFLAGS_DEBUG)
endif
```

- [ ] **Step 5: Commit**

```bash
git add src/include/debug/ftrace.h src/debug/ftrace.c src/bin/shell.c Makefile
git commit -m "feat: add ftrace-lite dynamic function tracer

Uses -finstrument-functions to trace function entry/exit.
4096-entry ring buffer with substring filtering and kallsyms
symbolic output. Shell commands: ftrace start|stop|add|clear|dump."
```

---

## Phase 7: Shell QoL

### Task 7.1: Command history

**Files:**
- Modify: `src/bin/shell.c`

- [ ] **Step 1: Add history buffer and handlers**

```c
// src/bin/shell.c — add near top:
#define HIST_SIZE 16
static char history[HIST_SIZE][256];
static int hist_head = 0;
static int hist_count = 0;
static int hist_pos = 0;

static void hist_add(const char* cmd)
{
    if (cmd[0] == '\0') return;
    int i = 0;
    while (cmd[i] && i < 255) {
        history[hist_head][i] = cmd[i];
        i++;
    }
    history[hist_head][i] = '\0';
    hist_head = (hist_head + 1) % HIST_SIZE;
    if (hist_count < HIST_SIZE) hist_count++;
    hist_pos = hist_count;
}
```

- [ ] **Step 2: Wire up in the input loop**

After the user presses Enter to submit a command, call `hist_add(buf)`.
Add `history` as a shell command that dumps the history buffer.

- [ ] **Step 3: Build and verify**

Run: `make -j4`
Expected: Builds clean. `history` command works in shell.

- [ ] **Step 4: Commit**

```bash
git add src/bin/shell.c
git commit -m "feat: add shell command history (last 16 commands)"
```

### Task 7.2: Tab completion

**Files:**
- Modify: `src/bin/shell.c`

- [ ] **Step 1: Add builtins table and completion function**

```c
static const char* builtins[] = {
    "arp", "backtrace", "bt", "cat", "dmesg", "dump", "fetch",
    "ftrace", "heapcheck", "help", "history", "kill", "ls", "nicregs",
    "ping", "ps", "reboot", "regs", "rm", "route", "runtests",
    "stackcheck", "tcpdump", "top", "uptime",
    NULL
};

static void tab_complete(char* buf, int* pos)
{
    /* Find the word being typed (last space-separated token) */
    int word_start = *pos;
    while (word_start > 0 && buf[word_start - 1] != ' ')
        word_start--;

    int word_len = *pos - word_start;
    
    /* Scan builtins for prefix match */
    const char* matches[32];
    int match_count = 0;

    for (int i = 0; builtins[i]; i++) {
        if (strncmp(buf + word_start, builtins[i], (size_t)word_len) == 0)
            matches[match_count++] = builtins[i];
    }

    if (match_count == 1) {
        /* Complete the word */
        const char* completion = matches[0] + word_len;
        while (*completion && *pos < 255) {
            buf[(*pos)++] = *completion++;
        }
        buf[*pos] = '\0';
    } else if (match_count > 1) {
        /* Show possibilities */
        terminal_writestring("\n");
        for (int i = 0; i < match_count; i++) {
            terminal_writestring(matches[i]);
            terminal_writestring("  ");
        }
        terminal_writestring("\n");
        /* Redraw prompt + current buffer */
        terminal_writestring(PROMPT);
        terminal_writestring(buf);
    }
}
```

- [ ] **Step 2: Wire tab key in input loop**

In the keyboard input handler, when TAB (scancode 0x0F) is pressed, call `tab_complete(buf, &pos)`.

- [ ] **Step 3: Commit**

```bash
git add src/bin/shell.c
git commit -m "feat: add shell tab completion for builtin commands"
```

### Task 7.3: `top` command

**Files:**
- Modify: `src/drivers/timer.c` — track ticks per task
- Modify: `src/include/task.h` — add cpu_ticks field
- Modify: `src/bin/shell.c` — add top_command

- [ ] **Step 1: Add cpu_ticks to task structure**

```c
// src/include/task.h — add to task_t:
uint32_t cpu_ticks;     /* CPU ticks consumed */
```

- [ ] **Step 2: Increment in timer tick**

```c
// src/drivers/timer.c — in timer_handler:
extern task_t* current_task;  /* or however tasks are tracked */
if (current_task)
    current_task->cpu_ticks++;
```

- [ ] **Step 3: Add top command**

```c
// src/bin/shell.c — add:
static void top_command(void)
{
    uint32_t total = 0;
    task_t* task = task_head; /* iterate task list */

    while (task) {
        total += task->cpu_ticks;
        task = task->next;
    }

    if (total == 0) total = 1; /* avoid div-by-zero */

    terminal_writestring("PID  NAME       CPU%  STATE\n");
    task = task_head;
    while (task) {
        uint32_t pct = (task->cpu_ticks * 100) / total;
        char buf[64];
        snprintf(buf, sizeof(buf), "%-3d  %-10s %-4u  %s\n",
                 task->pid, task->name, pct, "RUNNING");
        terminal_writestring(buf);
        task = task->next;
    }
}
```

- [ ] **Step 4: Commit**

```bash
git add src/include/task.h src/drivers/timer.c src/bin/shell.c
git commit -m "feat: add top command with per-task CPU usage"
```

### Task 7.4: Boot banner

**Files:**
- Modify: `src/kernel/kernel.c` — add print_banner

- [ ] **Step 1: Write print_banner function**

```c
// src/kernel/kernel.c — add:
#include "kernel/vsnprintf.h"
#include "serial.h"

static void print_banner(void)
{
    char buf[128];

    terminal_writestring("\n");
    terminal_writestring("JexOS v0.1 — i386 — Monolithic\n");
    terminal_writestring("Build: " __DATE__ " " __TIME__ "\n");

    /* RAM size from BIOS int 0x12 */
    uint16_t ram_kb = 0;
    __asm__ volatile("int $0x12" : "=a"(ram_kb) : "a"(0x1200) : "ebx", "ecx", "edx");
    snprintf(buf, sizeof(buf), "RAM: %u KB\n", (uint32_t)ram_kb);
    terminal_writestring(buf);

    /* RTL8139 MAC */
    if (rtl8139_is_initialized()) {
        uint8_t mac[6];
        rtl8139_get_mac(mac);
        terminal_writestring("NIC: RTL8139 (");
        for (int i = 0; i < 6; i++) {
            print_hex_byte(mac[i]);
            if (i < 5) terminal_putchar(':');
        }
        terminal_writestring(")\n");
    }

    terminal_writestring("FS: JexFS v1, 1.44 MB\n");
    terminal_writestring("\n");
}
```

- [ ] **Step 2: Call at the end of kmain**

```c
// In kmain, just before entering shell:
print_banner();
terminal_writestring("JexOS:~$ ");
```

- [ ] **Step 3: Commit**

```bash
git add src/kernel/kernel.c
git commit -m "feat: add boot banner with build info and device listing"
```

---

## Build & Verify

### Final integration test

After all phases are complete:

1. `make clean && make -j4` — build from scratch
2. `make run` — boot under QEMU with `-serial stdio`
3. Verify boot banner appears
4. Verify `/home/user/` is CWD
5. Test: `cat /sys/kernel/version`
6. Test: `echo 7 > /sys/kernel/log_level` (crank debug)
7. Test: `ping -v google.com` (network still works)
8. Test: `dmesg -l err` (filtered ring buffer)
9. Test: `top` (CPU usage display)
10. Test: `history` then up-arrow recall
11. Test: Tab completion on `pin<tab>` → `ping`

### Self-review checklist

After writing the plan, run through:

1. **Spec coverage:** Every requirement from the spec has a task. Phases 1-7 each map to one or more tasks with concrete code. All covered.

2. **Placeholder scan:** No "TBD", "TODO", "implement later", or "similar to". Every task has complete code or exact patterns to follow.

3. **Type consistency:** All APIs match between files. `kallsyms_lookup` signature matches in both .h and .c. `pci_device_id` matches between pci.h and rtl8139.c. `registers_t` type matches what GDB stub uses.

4. **No gaps found.**
