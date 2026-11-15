# JexOS Roadmap

The OS has evolved beyond the original v0.5 goals. Here's what's next.

## 🚧 In Progress — JexFS v3 (target: v0.8)

JexFS v3 is a format break, not a tuning pass. It changes the four things
that separate a major filesystem version from a point release: address
width, allocation model, consistency model, and integrity.

- **Address space** — 32-bit block numbers and 4 KB blocks (16 TiB ceiling)
  instead of 16-bit block numbers and 1 KB blocks (64 MB ceiling); 64-bit
  file sizes instead of 32-bit.
- **Allocation** — an extent tree in the inode instead of 8 direct + one
  single-indirect + one double-indirect block pointer.
- **Consistency** — a metadata journal with ordered data writes (ext3
  model), replayed at mount, instead of in-place metadata updates with no
  recovery path.
- **Integrity** — crc32c on every metadata block, a filesystem UUID,
  per-inode generation numbers, and a backup superblock.

Explicit non-goals for v3.0: snapshots, reflinks, compression,
encryption, quotas, multi-device.

| Phase | Work | Exit criterion | Status |
|---|---|---|---|
| 0 | Disk-first mount: probe LBA 0 or the delivery tail, mount the disk copy, return the module pages to the PMM (v2 format unchanged) | `free` recovers ~64 MB; audio, suite and delivery round-trip pass | [x] |
| 1 | `docs/jexfs-v3.md`, `mkjexfs3` + host verifier, read-only v3 mount | Host-built v3 image mounts and every file verifies byte-for-byte | [x] |
| 2a | Journal format, shared replay, ordered-data rule, deterministic crash simulation | Every truncation point of a logged transaction replays to the old or the new state, never torn; a mid-apply crash is repaired; `make crash-sim` green | [x] |
| 2b | Writable kernel path: allocation, metadata transactions, ordered data writes | Guest writes survive remount and a planted crash; `jexfsck3` clean | [x] |
| 3 | Extent tree, delayed allocation, hashed directories | Image larger than 64 MB; large-file metadata reads measurably lower | [ ] |
| 4 | v2→v3 converter, persistence rewrite, docs | Existing `jexos.img` converts; `make deliver` round-trips | [ ] |

Phase 1 is done. `src/include/jexfs3.h` defines the 4 KB-block format;
`tools/mkjexfs3.c` packs a tree (inline extents, with a 255-entry leaf
when a file needs more than four) and `tools/jexfsck3.c` validates both
superblocks, layout, inode/extent checksums, extent coverage and bounds,
dirent chains, link counts, bitmaps and data crcs, then compares every
file against its source byte-for-byte. The kernel reader in
`src/fs/jexfs3.c` mounts over a block callback, rejects unknown features,
replays a committed journal transaction at mount (revalidating the
superblock afterwards), and re-verifies every allocated inode and its
reachability from the root. `make check-v3` reports 5 files, 3
directories, 256 blocks, 0 failures; `make FLAVOR=test run-test` mounts
the same image from `/jexfs3.img`, passes the in-kernel consistency
check, reads files across an extent boundary, and walks the root
directory. A second kernel test copies a 48-block journaled image into a
RAM buffer, logs a two-block transaction, proves a torn copy is ignored,
proves the committed copy replays once, and proves a pending transaction
on a read-only device fails with `-EROFS`. A third kernel test replaces
inline-extent and leaf-extent files, truncates one to zero, remounts,
and reads the new contents back. `make crash-sim` replays all truncation
points of a real transaction, repairs a mid-apply crash, then crashes the
real file writer at every block-write boundary for two files and runs the
full verifier over all 57 resulting images. Flipping one byte in file
data or an inode makes the host verifier fail. Format details are in
`docs/jexfs-v3.md`.

## ✅ Completed Milestones

### Boot Performance — Disk-First Boot, IDE Fast-Fail, Measurement

Boot felt slow because two unrelated costs stacked up:

- **GRUB read the 64 MB rootfs module on every disk boot (~6.6 s).**
  `make run` / `run-debug` booted the CD ISO (which carries
  `/boot/jexos.img` as a multiboot module) *and* attached the disk.
  JexFS mounts the disk first and releases the module, so GRUB's read was
  pure waste. The run targets now boot a disk-only ISO
  (`build/run-<flavor>.iso`, no module); `make iso` (CD-only) still embeds
  it. Both staging paths come from one `iso_tree` make define, and every
  ISO build writes `GRUB_TIMEOUT` into `grub.cfg` instead of leaving a
  stale file behind.
- **An empty IDE channel burned four 10-million-iteration poll timeouts
  (~4.6 s).** With no disk attached, QEMU's bare PIIX channel returns
  `0x00` at status port `0x1F7`, not the `0xFF` the old early check tested
  for. `ide_wait_busy` / `ide_wait_drq` then spun their full timeout, four
  times over. `ide_init()` now probes once and caches `ide_present`, and
  both wait loops treat `0x00` or `0xFF` as "no drive" and return
  `-ENODEV` immediately; reads, writes and identify fail fast when the
  channel is empty. The probe polls for a few milliseconds first, so a
  drive that is still spinning up is not mistaken for an absent one.

Measurement build:

- `FLAVOR=boottime` is a debug clone plus a TSC timer anchored at the
  first kernel instruction and calibrated against the 100 Hz PIT (the
  calibration window is subtracted from the total). It prints
  `BOOTTIME: N ms (kernel entry -> shell ready)` on serial and powers off
  exactly when the shell prompt is up.
- `make run-boottime` boots it interactively with the normal GRUB menu;
  `make measure-boot` zeroes the menu timeout and reports host wall time
  from QEMU launch to poweroff, so GRUB's share is visible beside the
  kernel's.

Results under QEMU TCG:

| Scenario | Before | After |
|---|---|---|
| Disk rootfs (`make run`) | 7.87 s host total | 1.29 s host / 355 ms kernel |
| CD-only, no disk | 4654 ms kernel | 372 ms kernel |

The CD-only host total (~8.2 s) is still GRUB reading the 64 MB module;
that path cannot get faster without a smaller or compressed module, or a
disk. Verified: `make all`, `make check-v3` (0 failures), `make run-test`
(`SELFTEST PASS`, exit 0), and both measurement scenarios above.

### v0.8.0 — 64 MB Disk, Build-Time MP3 Transcode, AC'97 Underrun Fix

- [x] **Disk-First Rootfs (JexFS v3 Phase 0)**: `jexfs_mount_persistence()` probes the primary master before trusting the GRUB module. A JexFS superblock at LBA 0 (`make run`) or at `total_sectors - ramdisk_blocks*2` (the tail of a `make deliver` image) is validated against the drive capacity, and the mount switches block I/O to that copy. The module is then cold: its page-aligned interior goes back to `pmm_free_blocks()`, so the RAM it pinned is usable again. Measured on a 128 MB guest: 61,708 KB free at PMM init, 127,232 KB after the switch — 65,536 KB returned. `make run` boots a throwaway `build/run-jexos.img` copy (`run-debug` likewise) so guest writes no longer dirty the build artifact; `jexos.img` has the same md5 before and after a boot. Verified disk-mode selftest (exit 33), playback from disk (9.68 s of music in a 9.94 s capture, max RMS 21083), and a delivery round-trip: the first boot writes the tail, the second mounts the changed tail and still passes. No disk and a module still boots from the module exactly as before.
- [x] **64 MB Disk Image**: `IMG_BLOCKS ?= 65536` (64 MB at 1 KB blocks) is the format ceiling, not a preference — inode block numbers are `uint16_t` and the block bitmap is 8 blocks (65536 bits), so `mkjexfs` rejects anything larger. GRUB loads the image as a multiboot module so a diskless boot still works, but the module is only a fallback now: when an IDE disk is present the kernel mounts the on-disk copy and frees the module (see the disk-first bullet above). Verified `jexos.img` is exactly 67,108,864 bytes and every packed file compares byte-for-byte against its source.
- [x] **Build-Time MP3 Transcode**: the image rule stages `rootfs/` into `build/rootfs`, re-encodes each `rootfs/*.mp3` with ffmpeg (`-ar 44100`, libmp3lame, CBR 96 kbit/s, metadata stripped) and packs the result; `rootfs/` itself is never modified, so the originals stay untouched. `ROOTFS_MP3_BITRATE=<kbps>` overrides the rate. A missing or failing ffmpeg packs the original with a warning instead of failing the build, and the staging directory is wiped on every rebuild so deleted rootfs files cannot linger in the image. Verified with a 48 kHz source: build emits 44.1 kHz / 96 kbit/s (2,984,542 bytes), source md5 unchanged, image contents byte-verified.
- [x] **AC'97 Descriptor Ring**: CIV and LVI are 5-bit indices that wrap at 32, but the driver published an 8-entry list and wrapped its own arithmetic at 8 — past the first cycle the bus master fetched descriptors 8–31 out of bounds and halted at LVI. With `out.mixing-engine=off` the backend fills its whole period with silence as soon as the list stops, which is the music/silence alternation that was audible. The list is now 32 entries, LVI is an absolute index, only descriptors the ring can fill are queued, and the engine is left halted only when the ring is truly empty. Verified: a 4 s tone records as one continuous 4040 ms block, Get Lucky as 24.7 s of continuous music, and the 64 MB image yields 9.7 s of music in a 9.97 s capture.

### v0.7.9 — Selftest Suite, Build Flavors, Bug Harvest

- [x] **Rootfs Persistence Mirror**: `make deliver` builds `jexosg.img` = hybrid ISO + fresh rootfs appended at the tail; booted as an IDE hard disk, the kernel IDENTIFYs the drive, validates a JexFS superblock in the last `ramdisk_blocks` blocks, restores that mirror over the RAM rootfs (`jexfs_mount_persistence()`, process context after sti — the IDE mutex sleeps, so it cannot run from initcalls) and mirrors every `write_block()` back to the tail. (Superseded by the v0.8.0 disk-first mount, which mounts the tail directly and frees the module.) History and files now survive reboots from read-only media. Verified end-to-end: fingerprinted mirror restored over pristine module, suite write-through changed 9014 tail bytes, second boot passed on restored state; no-disk runs skip cleanly via the 0xFF floating-bus guard. `ide_init()` was previously never called anywhere — the probe is now its only user-driven init point
- [x] **Build Flavors**: `FLAVOR=debug|test` variants with separate object dirs and artifacts; debug cranks serial verbosity to everything, test replaces the shell with the suite
- [x] **In-Kernel Selftest Runner**: registered suites announce `[RUN]` before executing (a hang names its test), results mirror to serial, and the exit code goes out the isa-debug-exit port — `make run-test` streams live output headless and fails on 7 (fail) or timeout (hang); pass is 33 because qemu arg errors also exit 1
- [x] **Test Suites**: 30 tests — heap classes/double-free/OOM, ringbuf wrap/full-drop, SMP atomic hammer via `kernel_fork_on`, mutex exclusion, completion early-signal, memops/snprintf truncation+formats, JexFS 9KB roundtrip/remove-missing/concurrent writers, SMP counter/diverge/torture, kernel_fork stress, page fault in kernel, per-task FPU isolation/template, MP3 stereo+mono decode
- [x] **kheap Lock Leaks**: kmalloc OOM path and kfree invalid-pointer path returned holding `kheap_lock` with IRQs off — later allocations froze forever (the mystery hang)
- [x] **Slab Double-Free Detection**: free_count only catches whole-slab over-freeing, so a same-object double free aliased two freelist entries; per-slab in-use bitmap rejects it and corrupt freelists outright
- [x] **Per-Task ISR Depth**: the global `in_isr` lied across preemption — a victim frozen in `irq_handler` left it stranded positive and unrelated tasks failed the can-sleep assert; every task counts its own depth now, the global only gates printk
- [x] **JexFS Dir Scanners**: zeroed entries were end-of-directory, hiding anything appended past a gap (create ok, open/remove ENOENT); scanners step holes and clamp to the next block boundary after each walk (fixes a livelock too); list_dir skips holes instead of truncating; read/write_block reject out-of-range blocks instead of issuing raw IDE sectors

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
- [x] **Command History**: `.history` saved and reloaded within a session; on the delivery image (`make deliver`, IDE hard disk boot) it survives reboots (the tail is now mounted directly — see v0.8.0)
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
- [x] **lockdep**: Lock ordering validator (rewritten: per-task held stacks,
  name-keyed edge table, BFS cycle query, unit-tested). Ships compiled-in but
  default-OFF via `lockdep_enabled`; shell `lockdep` toggles it live.
- [x] **ftrace-lite**: Dynamic function tracing
- [x] **GDB Stub**: Remote debugging over serial
- [x] **`sym <addr>`**: resolve hex addresses to symbol+offset/size (kallsyms)
- [ ] **lockdep always-on crash**: with hooks live on the mutex hot path the
  selftest suite dies in `mutex_exclusion` (GP/PF, exit 124); bisected past
  validation and BFS — plain slot claim + push/pop bookkeeping alone still
  kills it. Fingerprint: faulting frames consume half-words of *stack/return
  addresses* as segment selectors (CS=0xDA89 ← ret 0x11DA89; err=0xFFA4 ←
  stack addr 0xFFC2FFA4), i.e. something in the switch/trampoline path
  misparses frame contents once real acquire/release bodies run. Hooks off or
  stubbed = suite green; layout verified collision-free in the link map.
  Next probe: serial breadcrumbs at lockdep entry/exit + cur_task() identity
  per call on a debug iso, ideally on real 2-CPU hardware.

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
- [x] **JexFS**: disk-backed rootfs with hierarchy; `make deliver` appends it at the tail and the kernel mounts that tail directly (v0.8.0; the earlier RAM image + write mirror is gone) — history and files survive reboots from read-only media
- [x] **VFS Layer**: Mount point dispatch
- [x] **devtmpfs/sysfs**: Virtual filesystems
- [x] **IDE PIO Driver**: ATA disk access; PIO transfers hold a mutex instead of cli — contending tasks sleep while timers keep ticking on both cores; sector cache on its own short spinlock with in-progress fill marks
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
- [x] **TLB Shootdown IPI**: `free_kernel_stack()` clears PTEs, then IPIs every other online CPU (vector 49, handler does a full CR3-reload flush and acks) and only returns frames to PMM after all acks land — stale remote translations can no longer alias recycled kstack memory into a new owner's pages. Single-CPU builds skip the broadcast entirely.
- [ ] **PID 2 Boot Stack Page**: give PID 2 an alloc'd kernel stack page (0xFFC08000) and update `set_kernel_stack()`/TSS.ESP0, so ISR frames never land on the real boot stack. This eliminates the entire class of iret corruption bugs on SMP.
- [ ] **IPI Reschedule**: wake a CPU's scheduler remotely instead of polling
- [ ] **Per-CPU `%gs`**: cache the CPU index / per-CPU pointer instead of an MMIO LAPIC ID read + scan in every `cpu_id()`
- [ ] **Remaining shootdown scope**: generic page-table unmaps (`paging_unmap_page` userspace paths) still flush locally only; wire them once ring-3 makes remote aliasing reachable.
- [x] **Kernel FPU/SSE State**: eager per-task `FXSAVE`/`FXRSTOR` (512-byte, 16-byte-aligned image; `FNSAVE`/`FRSTOR` when CPUID has no FXSR), wired into `task_switch()` and AP bring-up, with `CR0.MP=1`/`EM=0` and `CR4.OSFXSR|OSXMMEXCPT`. A new task starts from the pristine `fninit` image, so it cannot inherit another task's control word. The no-float-in-IRQ rule is documented in `fpu.h`; the kernel stays `-mno-80387` and only `drivers/mp3.o` opts back in with `-m80387`. Still open: ring-3 float from TCC is not exercised by the suite, and there is no `#NM`/`CR0.TS` lazy path — eager save/restore is the whole policy.
- [x] **LAPIC Timer**: per-CPU tick source armed after PIT calibration; PIT keeps wall-clock duty only
- [x] **Per-CPU TSS**: GDT grew one TSS slot per core, each AP LTRs its own at startup — ring-3 interrupts work off-BSP
- [ ] **Page-Directory Teardown on Reap**: waitpid still leaks cloned address spaces (see TODO in task.c)
- [x] **Atomic Console Writes**: log_serial locks whole messages; serial output is line-clean with both cores logging
- [ ] **SMP Runtime Soak**: exercise fork-heavy loops, Ctrl+C storms and `top` on both cores for a while before trusting it (ring-3 FPU state and the PID 2 boot stack above are the known sharp edges)
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

### Audio
- [ ] **Speaker Driver v2 (blocking → timer-driven)**: `beep()` is `play_sound(); sleep(ms); stop_sound();`, and `sleep()` is an `hlt` spin counted in ticks — so the calling task sits in `beep()` for the whole note and the shell takes no input until the tune ends. Needs a note queue advanced from the tick handler so `beep()` returns immediately.
- [x] **PCM Output Driver**: AC'97 (`src/drivers/ac97.c`), 44.1 kHz s16 stereo over PCI bus-master DMA. The PC speaker is a 1-bit square wave (PIT ch2 → port 0x61) — no amplitude, no sample clock, so it could never play samples. `sndtest` plays a four-note arpeggio. Two warts worth knowing: completion is polled from the PIT tick because QEMU puts AC'97 and RTL8139 on the same IRQ line and `irq.c` allows one handler per line (the list is 32 entries because CIV/LVI are 5-bit — see v0.8.0 above); and `out.mixing-engine=off` in `QEMU_OPTS` is what keeps the stream at real time (see the comment there).
- [x] **MP3 Playback**: `play <file>` decodes MPEG-1 Layer III from JexFS into AC'97 (`src/drivers/mp3.c`, streaming minimp3 vendored at `third_party/minimp3.h`, CC0). The decoder is float, so it is the one translation unit built with `-m80387`; everything else stays integer-only, and that is safe only because of the per-task FPU image above. Scope: 44.1 kHz, 1 or 2 channels (mono duplicated to stereo), CBR or VBR frames decoded as they arrive. Other sample rates and channel layouts are refused (`-EINVAL`) rather than played at the wrong pitch; no resampling, no gapless metadata. Files dropped in `rootfs/` are transcoded to this format at image-build time (see v0.8.0), so the common case never reaches the refusal path. Verified by in-kernel stereo/mono decode tests (rate, length, level, duplication) and a WAV capture of `play /test.mp3` containing the 440/660/880/1100 Hz arpeggio.

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
