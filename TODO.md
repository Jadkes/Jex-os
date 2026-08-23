# JexOS Roadmap

The OS has evolved beyond the original v0.5 goals. Here's what's next.

## ✅ Completed Milestones

### master — Blocking Sync, FS Serialization, Fault Reporting

- [x] **Sleeping Mutexes**: `mutex_t` parks losers on a FIFO under the scheduler (new `STATE_BLOCKED` skipped by every pick loop), atomic fast path, direct ownership handoff on unlock, zombie waiters dropped so unlock can't resurrect corpses; `sys_kill` un-parks its target so SIGKILL reaches blocked tasks
- [x] **Completions**: task↔ISR handshake primitive — signal from any context wakes all waiters, first consumes the flag, signal-before-wait is not lost (no driver uses one yet; IDE still PIO-polls)
- [x] **Atomic API**: `kernel/atomic.h` wraps the __sync builtins (`inc/dec/add/xchg/cmpxchg/or/and`) on `uint32_t`; migrated isr depth, lapic timer count, keyboard/sigkill signal races and rtl8139 packet counters off raw builtins
- [x] **SPSC Ring Buffer**: `ringbuf.h` byte queue (producer owns head, consumer owns tail, power-of-2 mask); keyboard driver rebuilt on it
- [x] **IDE Sleeping Lock**: PIO transfers hold a mutex instead of cli — contending tasks sleep while timers keep ticking on both cores; sector cache on its own short spinlock with in-progress fill marks
- [x] **JexFS Coarse Lock**: one mutex over all metadata ops (`_nolock` cores + wrappers); ls/tab-completion hold it across whole scans via `jexfs_read_inode_raw()` — concurrent create/remove can no longer tear walks or bitmaps
- [x] **Sleep Asserts**: mutex_lock from an ISR or irqs-off region panics naming the lock instead of deadlocking silently
- [x] **PID Atomics**: next_pid via `atomic_inc_return` everywhere — duplicate ids impossible by construction, boot ordering no longer load-bearing
- [x] **Double-Fault Landing Pad**: vector 8 is a task gate to a dedicated TSS with a private 16k stack and kernel segments; reports over raw serial with CR2 and halts instead of triple-faulting mute; paging hands it the kernel directory
- [x] **Scheduler Runnable Fix**: pick loops required STATE_READY but executing tasks are STATE_RUNNING across preemption — first timer tick ever hlt'd forever mid-boot; `task_is_runnable()` accepts both, verified by full two-core boot (armed=2, self-test 5/5)
- [x] **i486 Baseline**: -march=i386 has no cmpxchg so general __sync ops became unlinked libgcc calls; i486 makes them all inline locked instructions (cpuid was already required for SMP)

### master (post-v0.7.4) — Real SMP Scheduling, Allocator Hardening, Serial Boot Log

### True SMP
- [x] **Spinlocks**: PMM bitmap, kheap slabs, kernel-stack slots, PID handout and the task lists serialize on real locks (`spinlock.h`), not IF games
- [x] **Per-CPU TSS**: GDT grew one TSS slot per core (`GDT_BASE_ENTRIES + NR_CPUS`); each AP LTRs its own at startup, so ring-3 interrupts work off-BSP
- [x] **LAPIC Timer**: calibrated against PIT ticks on the BSP (interrupt-forced measure window), go-flag handshake arms every core's own periodic timer; PIT keeps wall-clock duty only
- [x] **APs Run the Scheduler**: seeded idle task reuses the AP boot stack; park loop pause-spins (hlt would never wake at IF=0), then sti and round-robin its queue
- [x] **Load Spreading**: `fork`/`kernel_fork` place children on the shortest ready queue, cross-cpu
- [x] **Global Tasking Correctness**: one `tasklist_lock` covers switch/fork/exit-reparent/waitpid-reap/kill/pid-lookup across all CPUs' queues; signal bits set atomically against the keyboard ISR; shell `top` scans under `tasks_lock`
- [x] **Stack-Slot Recycling**: `free_kernel_stack()` derives the slot from the stack's virtual address — waitpid can free stacks handed out by any CPU, no more fixed per-pid slots dying past 128 tasks

### Allocator & Driver Hardening (bug audit)
- [x] **rtl8139 Drain Unification**: ISR and poll paths share `rtl8139_drain_rx()` — one full lap past wraps, CAPR written with the documented gap, frame copies capped at RX_MAX_FRAME
- [x] **jexfs Block-End Guard**: dir_insert requires the entry to fit before writing; write_entry backs it with -EIO instead of straddling into the next block
- [x] **IP Length Validation**: handle_ip rejects frames whose ip_tot_len is smaller than the header or larger than what arrived; tcp sanity-checks the segment offset
- [x] **slab_free Validation**: negative/misaligned offsets, out-of-range indices and double frees are rejected instead of silently corrupting the bitmap

### Boot Diagnostics
- [x] **Serial-Only Boot Log**: `pr_serial()` writes linux-style boot lines (multiboot info, memory totals, cpu/timer status, NIC/IP/FS summary) to `-serial stdio` while VGA output stays byte-for-byte unchanged
- [x] **Console Lock**: log_serial holds a lock per message — no more torn lines from BSP/AP racing on the wire
- [x] **Atomic ISR Depth Counter**: `in_isr` increments atomically now that every core dispatches IRQs

### master (post-v0.7.4) — SMP, IO/APIC, Clock Fixes

### Interactivity & Hardening
- [x] **Ctrl+C → SIGINT**: keyboard ISR signals the shell's foreground child (`fg_child_pid` set around waitpid); ^C at the prompt abandons the line
- [x] **cpuinfo command**: CPUID vendor/family/features plus per-core LAPIC id, online state and current task
- [x] **Per-CPU top**: per-core summary block (lapic id, BSP/AP, ready depth, running task)
- [x] **IDE Sector Cache**: 64-line LRU cache in front of PIO reads, write-through updates, IRQ-guarded
- [x] **Syscall Pointer Audit**: user buffers/strings/argv validated for range AND page mapping before kernel dereference; signal handler pointers bounded; fork debug print removed

### SMP Bring-up
- [x] **ACPI Discovery**: checksum-validated RSDP scan (EBDA → FSEG spec order), MADT parse for CPUs/LAPIC/IOAPIC
- [x] **AP Wake**: real-mode trampoline at 0x1000, INIT-SIPI sequence with PIT-calibrated delays, second SIPI only if the AP missed the first
- [x] **Per-CPU State**: `cpus[]` with per-CPU ready queues, current task, kernel stacks, cloned page directories
- [x] **IO/APIC Ownership**: 8259 masked, all IRQs through the IOAPIC; PCI INTx routed via PIIX3 PIRQ cookies to GSI 16–19
- [x] **cpu_id() Safety Net**: logs once if a CPU's LAPIC ID has no cpus[] entry

### Clock & Timing
- [x] **Cascade-line PIT Fix**: QEMU's PIIX3 delivers PIT ticks on GSI 2 (vector 34), not GSI 0 — timer registered on both slots
- [x] **Rate-generator Mode**: mode 3's square wave double-delivered on the cascade path (~2× tick rate); mode 2 gives one edge per tick
- [x] **Boot Self-test**: delivered ticks counted against a 50 ms channel-2 stopwatch every boot
- [x] **sleep() IF Hygiene**: hlt requires interrupts enabled; caller state captured and restored around the wait

### Robustness
- [x] **irq_save/irq_restore**: EFLAGS-capturing interrupt guards replace bare cli/sti across net/tcp/timer/rtl8139 paths reachable from ISR context
- [x] **TX Descriptor Lock**: rtl8139 send packet claim is atomic vs. ISR-triggered ACK sends
- [x] **in_isr Nesting Counter**: nested handlers no longer un-suppress outer frames' VGA output
- [x] **UDP Dispatch Under Lock**: callbacks run with IF=0 instead of nesting interrupts mid-ISR
- [x] **top Accounting Fix**: queued-current task counted once, percentages sum correctly
- [x] **Named Hardware Constants**: PIT/PIC/VGA/UART/GDT/syscall magic numbers replaced tree-wide

### v0.6.5 — TCP Hardening, HTTP Server, DHCP Client

### TCP & Network Stack
- [x] **Runtime IP Configuration**: `OUR_IP`, `GATEWAY_IP`, `DNS_SERVER` as mutable globals
- [x] **Passive TCP Open**: `tcp_listen()` / `tcp_accept()` — LISTEN → SYN_RCVD → ESTABLISHED
- [x] **TCP State Expansion**: Added LISTEN (8) and SYN_RCVD (9) states
- [x] **MSS Option**: Advertised on SYN segments
- [x] **TCP Checksum**: Proper pseudo-header checksum on all segments
- [x] **Zero Compiler Warnings**: All `-Wall -Wextra` issues fixed in `tcp.c`
- [x] **Connection Filtering**: Source IP/port matching for established connections

### HTTP Server
- [x] **`serve` Command**: Listens on port 80, accepts connections
- [x] **HTTP/1.0 Response**: Content-Type, Content-Length headers
- [x] **JexFS File Service**: Serves files from the filesystem
- [x] **Default Fallback Page**: "Hello from JexOS!" HTML
- [x] **QEMU hostfwd**: Makefile updated for host→guest port forwarding

### DHCP Client
- [x] **DORA Cycle**: Discover → Offer → Request → Ack
- [x] **UDP Broadcast**: Manual packet construction for 255.255.255.255
- [x] **TLV Option Parsing**: Extracts IP, gateway, DNS from DHCP ACK
- [x] **`dhcp` Command**: One-shot network auto-configuration
- [x] **Retry Logic**: 3 retries per message type with timeout
- [x] **`dhcp.h` Header**: Protocol constants and packet struct

### Shell & UX
- [x] **Paged Help**: `more`-style pagination (22 lines per page)
- [x] **`dhcp` in Command List**: Tab-completion and help entry
- [x] **`serve` in Command List**: Tab-completion and help entry
- [x] **Updated `net` Command**: Shows IP/Gateway/DNS config

### v0.6.0 — Slab Allocator, Signals, Keyboard Buffer
- [x] **Slab Allocator**: 8 power-of-2 size classes (16B–2048B), O(1) kfree
- [x] **PMM Fallback**: Handles missing multiboot memory map
- [x] **Signal Handling**: 32 signal slots per task, sys_signal/sys_kill
- [x] **Keyboard Ring Buffer**: ISR decoupled from shell, no dropped keys
- [x] **Ctrl+L**: Clear screen shortcut

### Shell & UX
- [x] **Tab Completion**: Auto-complete filenames and commands
- [x] **Command History**: `.history` saved and reloaded within a session (RAM-backed fs: it dies with the power)
- [x] **Dynamic Prompt**: Color-coded, directory-aware
- [x] **Uptime**: System uptime command
- [x] **Top**: Per-task CPU usage monitor
- [x] **Boot Banner**: Build info + device listing on startup
- [x] **Ctrl+L**: Clear screen shortcut

### Debug Suite
- [x] **Panic Handler**: Crash screen, register dump, stack trace, page fault decode
- [x] **Kernel Ring Buffer** (`dmesg`): Structured logging with severity
- [x] **Backtrace** (`bt`): Live stack unwinding
- [x] **Hexdump** (`dump`): Memory inspection
- [x] **ASSERT / WARN_ON**: Runtime assertion checking
- [x] **Test Framework** (`runtests`): In-kernel unit tests
- [x] **Heap/Stack Inspector** (`heapcheck`, `stackcheck`)
- [x] **lockdep**: Lock ordering validator
- [x] **ftrace-lite**: Dynamic function tracing
- [x] **GDB Stub**: Remote debugging over serial

### Network Stack
- [x] **RTL8139 Driver**: DMA ring buffer, interrupt-driven
- [x] **ARP Cache**: Address resolution with timeout
- [x] **IP + ICMP**: Ping responder and originator
- [x] **UDP**: Send and receive with port registration
- [x] **TCP Client**: Connection, HTTP GET (`fetch`)
- [x] **DNS Resolver**: Hostname lookup with retry
- [x] **Gateway Routing**: Non-local traffic via 10.0.2.2
- [x] **PCI Driver Model**: Auto-probe with initcall registration
- [x] **arp/netlog/tcpdump/nicregs**: Network diagnostics

### Storage
- [x] **JexFS**: RAM-backed rootfs with hierarchy (image reloads every boot; flushing to the hda data disk is open work)
- [x] **VFS Layer**: Mount point dispatch
- [x] **devtmpfs/sysfs**: Virtual filesystems
- [x] **IDE PIO Driver**: ATA disk access
- [x] **Standard commands**: `cp`, `mv`, `rm`, `ls`, `cat`, `mkdir`, `cd`

### Kernel Infrastructure
- [x] **Initcall Framework**: Automatic driver/subsystem initialization
- [x] **Multitasking**: Round-robin scheduler
- [x] **Memory Isolation**: Private page directories per process
- [x] **kallsyms**: Symbol table for backtraces
- [x] **vsnprintf/snprintf**: Safe formatted output
- [x] **Workqueue**: Deferred execution

---

## 🎯 Current Goals

### SMP Follow-ups
- [x] **Spinlocks / Mutexes**: PMM bitmap, kheap slabs, kstack slots, PID handout and the task lists now run on real spinlocks (`spinlock.h`), not IF games
- [x] **Load Balancing**: fork/kernel_fork place new tasks on the shortest ready queue; APs wake into a seeded idle task and preempt off their own LAPIC timer
- [ ] **IPI Reschedule**: wake a CPU's scheduler remotely instead of polling
- [ ] **Per-CPU `%gs`**: cache the CPU index / per-CPU pointer instead of an MMIO LAPIC ID read + scan in every `cpu_id()`
- [ ] **TLB Shootdown IPI**: `free_kernel_stack()`/page-table edits only `invlpg` locally — a stale remote TLB can alias freed frames once memory is recycled across CPUs
- [ ] **FPU/SSE State**: no fxsave/frstor or #NM handler — float code in two interleaved tasks corrupts state (pre-existing, worse with SMP)
- [x] **LAPIC Timer**: per-CPU tick source armed after PIT calibration; PIT keeps wall-clock duty only
- [x] **Per-CPU TSS**: GDT grew one TSS slot per core, each AP LTRs its own at startup — ring-3 interrupts work off-BSP
- [ ] **Page-Directory Teardown on Reap**: waitpid still leaks cloned address spaces (see TODO in task.c)
- [x] **Atomic Console Writes**: log_serial locks whole messages; serial output is line-clean with both cores logging
- [ ] **SMP Runtime Soak**: exercise fork-heavy loops, Ctrl+C storms and `top` on both cores for a while before trusting it (FPU corruption and TLB shootdown above are the known sharp edges)
- [ ] **Wire Completions to Hardware**: completion_t exists but nothing signals one; IRQ-driven IDE transfers would be the first real user
- [ ] **Kernel-Resident Apps**: ping/serve/vix live in kernel space where a bug takes the machine down — migrate to the ring-3 user/ tree (needs console syscalls, then ELF injection into the rootfs image)
- [ ] **Userspace Socket API**: networking is kernel-builtin only; syscalls or a socket fs before ring-3 programs can speak IP
- [ ] **Serial Input Path**: UART RX has no IRQ handler or ring — the serial console is output-only

### Networking
- [ ] **TCP Window Scaling**: Advertise and handle window sizes
- [ ] **Checksum Validation**: Verify on receive, not just on send
- [ ] **Concurrent Connections**: Multiple sockets
- [ ] **TLS / HTTPS**: Because plaintext is 1995

### Shell & Utilities
- [ ] **JexSnake**: Terminal snake game
- [ ] **Pipes & Redirection**: `|`, `>`, `<` operators
- [ ] **Command Aliases**: `alias ll='ls -l'`
- [ ] **Grep**: Text search within files
- [ ] **Find**: File search by name
- [ ] **Environment Variables**: `PATH` support

### System Improvements
- [x] **Slab Allocator**: Power-of-2 size classes, O(1) kfree, PMM fallback
- [x] **Keyboard Buffering**: Ring buffer, no dropped keys under load
- [x] **Signal Handling**: sys_signal/sys_kill, delivery in scheduler
- [x] **SMP**: two CPUs booting, per-CPU run queues, IOAPIC routing (see master milestone above)
- [ ] **TFTP Fetch**: pull files into JexFS from QEMU's built-in TFTP server — no hostfwd gymnastics
- [ ] **Boot CI**: GitHub Actions builds the ISO on every push and boots it headless; fail if the clock self-test line is wrong
- [ ] **fork-per-connection serve**: combine fork() with passive-open TCP for concurrent HTTP
- [ ] **PS/2 Mouse Driver**: QEMU emulates it; a text-mode cursor is the gateway drug to GUI
- [ ] **AHCI / SATA**: Faster disk I/O
- [ ] **USB Keyboard/Mouse**: Beyond PS/2

### Deep End
- [ ] **Grow TCC toward real C**: pointers and structs would let the OS compile non-toy programs of itself
- [ ] **VESA/VBE Graphics**: 640x480 framebuffer, then a logo splash or tiny paint app
- [ ] **HPET / TSC-deadline Clockevent**: retire the PIT entirely, microsecond sleeps
- [ ] **Stack Guard Pages & Canaries**: neither exists yet; overflow today smashes adjacent memory silently
- [ ] **Screensaver**: starfield in text mode with block glyphs. Pure fun, zero risk

### Polish
- [x] **Ctrl+L**: Clear screen shortcut
- [x] **Music Tempo**: play_tune rescored for the now-correct clock
- [ ] **Scrollback**: Terminal history buffer
- [ ] **Color Themes**: Customizable palette
