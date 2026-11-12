# JexOS

A 32-bit x86 hobby OS written from scratch: freestanding C,
a full network stack, symmetric multiprocessing, multitasking, a
filesystem, and a self-hosting TCC compiler.

## Screenshots

![Pinging google.com from JexOS](assets/jexos-ping.png)

![Editing a C file with vix inside JexOS](assets/jexos-vix.png)

## Features

- **Network stack** — ARP, IP, ICMP, UDP, TCP from scratch; RTL8139 driver
  (DMA ring buffer, interrupt-driven)
- **HTTP** — `serve` (TCP server) and `fetch` (client); DHCP and DNS clients
- **Compiler** — self-hosting TCC with `if`/`else`, `while`, `for`, and hex printing
- **Filesystem** — JexFS on a 4 MB RAM-backed root image; hierarchy, proper errno codes; persistence mirror via `make deliver` (survives reboots from read-only media)
- **Multitasking** — round-robin scheduler, `fork`/`waitpid`, signals, per-process page tables
- **SMP** — both CPUs run the scheduler: ACPI MADT discovery, INIT-SIPI bring-up,
  per-CPU run queues with load-spreading forks, per-CPU LAPIC timer preemption,
  spinlock-guarded allocators and task lists, one TSS per core, IOAPIC routing,
  TLB shootdown IPIs for safe kstack recycling
- **Synchronization** — sleeping mutexes with a wake-one wait list, completions for
  task↔ISR handshakes, spinlocks for short sections, atomics everywhere shared
  state crosses CPUs; illegal sleeps (mutex under ISR/irqs-off) panic loudly
  instead of deadlocking quietly
- **Shell** — tab completion, command history, `ps`/`kill`/`top`, vix editor
- **Debugging** — panic screen with register dump and stack trace, `dmesg`,
  GDB stub, ftrace-lite; a dedicated-stack double-fault landing pad
  reports crashes that would otherwise reset silently; linux-style boot log on
  the serial console (`-serial stdio`) while VGA output stays clean; the debug
  flavor adds a spinlock watchdog that prints `[[ SPINCONTEND @ addr ]]` the
  first time a lock word is contended and repeats `[[ SPINLOCK STUCK @ addr ]]`
  while a contender spins too long — reports go straight to the uart with no
  console lock, so they survive from any context and name the exact lock;
  `sym <addr>` resolves any hex address to `symbol+offset/size` via the kernel
  symbol table; the lock-order validator ships compiled-in but inert — flip it
  live with the shell's `lockdep` command to start checking every mutex
  acquire against the recorded ordering table

## Build & Run

```bash
make              # build kernel + disk + boot image
make run          # boot in QEMU via GRUB (default)
make run-native   # boot via the custom two-stage bootloader
make iso          # build the GRUB ISO only
make FLAVOR=debug iso   # verbose serial boot log
make FLAVOR=test iso    # build the self-test suite
make run-test    # headless qemu; exit 33=pass 7=fail 124=hang
```

Boots with GRUB by default, which gives the kernel a proper memory map
and keeps the JexFS data disk on hda. `make run-native` instead boots
through JexOS's own stage-1/stage-2 bootloader
(BIOS → bootsect → loader → protected mode).

Requires `gcc` (i386), `binutils`, `python3`, `grub2-tools`, and
`qemu-system-i386`.

## Roadmap

- [x] Self-hosting TCC
- [x] TCP server (passive open)
- [x] SMP scheduling on all CPUs (spinlocks, per-CPU timers, load spreading)
- [x] JexFS persistence: `make deliver` builds a hybrid ISO with rootfs appended at the tail; kernel restores it on boot, mirrors writes back — history and files survive reboots from read-only media
- [x] TLB shootdown IPIs: `free_kernel_stack()` flushes remote TLBs via IPI before returning frames to PMM
- [ ] PID 2 boot stack page: give the BSP task a proper kernel stack page so ISR frames never land on the real boot stack (eliminates SMP iret corruption class)
- [ ] Per-task FPU state: the kernel is integer-only by construction (`-mno-80387` turns any kernel float into a link error, since nothing saves x87 state across a switch); ring-3 float still needs a per-task FXSAVE area and `#NM` handling
- [ ] Pipes & redirection

## Status

v0.7.9: the kernel now tests itself. `make FLAVOR=test iso` boots
straight into an in-kernel suite — 26 tests over the allocator, ring
buffer, sync primitives, printf, JexFS, and SMP stress — and reports
pass/fail through the isa-debug-exit port so `make run-test` fails hard
on any regression, hang or panic. Writing the suite paid for itself
immediately: it caught two kheap lock leaks, undetectable slab double
free, a stranded ISR depth counter that framed innocent tasks for illegal
sleeps, JexFS directory scanners that treated zero padding as end-of-
directory, and a series of real SMP bugs — tasklist lock deadlocks under
TLB shootdown IRQ windows, mutex handoff orphaning the lock token, and
iret frame corruption from nested interrupts on PID 2's boot stack.

Persistence works. `make deliver` appends the rootfs at the tail of the
ISO; booted as an IDE hard disk, the kernel identifies the drive,
validates the superblock, and restores the mirror over the RAM rootfs on
every boot. Every `write_block()` is mirrored back, so history and files
survive reboots from read-only media.

SMP: both CPUs boot, arm their own LAPIC timer, run the scheduler, and
spread load via shortest-queue forks. The root causes found across this
investigation — waitpid freeing a kstack under tasklist_lock, mutex
handoff orphaning the lock token, completion lost wakeups, a kstack
overwrite in kernel_main, nested iret corruption, and the naked
task_switch window — are all fixed. One known limitation remains: PID 2 runs on the real boot
stack (not an alloc'd kernel stack page), so on SMP the boot stack can
still be corrupted by a nested timer ISR. The architectural fix
(giving PID 2 a proper kernel stack page) is the next priority.

`FLAVOR=debug` builds the everything-on serial log for chasing the rest.

Earlier: the kernel grew real blocking synchronization. Mutexes park
losers on one wait list and wake exactly one on unlock (no thundering
herd); the woken task re-contends rather than inheriting ownership, which
is what keeps the token from being orphaned. Completions cover
task↔ISR events, JexFS metadata runs under one coarse sleeping lock so
concurrent file operations cannot tear the bitmaps, and IDE transfers
hold a mutex instead of freezing every timer on the machine. PIDs come
from an atomic counter, the scheduler treats READY/RUNNING uniformly, and
vector 8 lands on its own stack to log double faults instead of rebooting
mute.

v0.7.4: boot via GRUB by default (custom two-stage loader kept as
`make run-native`), no hardcoded RAM cap (bitmap covers the full 4GB
address space), and `free`/memory stats report the RAM actually
detected by the bootloader instead of nonsense.

v0.7.0: TCC self-hosting, control flow, flat filesystem. TCC is experimental
and buggy — report any that you find.

## Contributing

Educational project. Network protocol bugs, driver improvements, and shell
features are all welcome.
