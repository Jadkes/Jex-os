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
- **Filesystem** — JexFS on a 4 MB RAM-backed root image; hierarchy, proper errno codes
- **Multitasking** — round-robin scheduler, `fork`/`waitpid`, signals, per-process page tables
- **SMP** — both CPUs run the scheduler: ACPI MADT discovery, INIT-SIPI bring-up,
  per-CPU run queues with load-spreading forks, per-CPU LAPIC timer preemption,
  spinlock-guarded allocators and task lists, one TSS per core, IOAPIC routing
- **Synchronization** — sleeping mutexes with FIFO handoff, completions for
  task↔ISR handshakes, spinlocks for short sections, atomics everywhere shared
  state crosses CPUs; illegal sleeps (mutex under ISR/irqs-off) panic loudly
  instead of deadlocking quietly
- **Shell** — tab completion, command history, `ps`/`kill`/`top`, vix editor
- **Debugging** — panic screen with register dump and stack trace, `dmesg`,
  GDB stub, ftrace-lite, lockdep; a dedicated-stack double-fault landing pad
  reports crashes that would otherwise reset silently; linux-style boot log on
  the serial console (`-serial stdio`) while VGA output stays clean

## Build & Run

```bash
make              # build kernel + disk + boot image
make run          # boot in QEMU via GRUB (default)
make run-native   # boot via the custom two-stage bootloader
make iso          # build the GRUB ISO only
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
- [ ] JexFS persistence: flush dirty blocks to the hda data disk (the rootfs
      currently resets from the multiboot module every boot)
- [ ] Pipes & redirection
- [ ] TLB shootdown IPIs and FPU state saving between CPUs

## Status

v0.7.9: the kernel now tests itself. `make test` boots straight into an
in-kernel suite — 21 tests over the allocator, ring buffer, sync
primitives, printf and JexFS — and reports pass/fail through the
isa-debug-exit port so `make run-test` fails hard on any regression,
hang or panic. Writing the suite paid for itself immediately: it caught
two kheap lock leaks, undetectable slab double frees, a stranded ISR
depth counter that framed innocent tasks for illegal sleeps, and JexFS
directory scanners that treated zero padding as end-of-directory.
`FLAVOR=debug` builds the everything-on serial log for chasing the rest.

master: the kernel grew real blocking synchronization. Mutexes park losers
on a FIFO and hand off directly on unlock (no thundering herd), completions
cover task↔ISR events, JexFS metadata runs under one coarse sleeping lock so
concurrent file operations cannot tear the bitmaps, and IDE transfers hold a
mutex instead of freezing every timer on the machine. PIDs come from an
atomic counter, the scheduler treats READY/RUNNING uniformly, and vector 8
lands on its own stack to log double faults instead of rebooting mute.

Earlier: SMP is real. Both CPUs wake through the trampoline, seed an idle
task, arm their own LAPIC timer (calibrated against the PIT) and preempt
their own ready queues; fork spreads children onto the shortest queue and
waitpid reaps zombies across every CPU. Shared allocators and the task
lists run on spinlocks, each core owns a GDT/TSS slot so ring 3 works
everywhere, and the boot now narrates itself on the serial console —
memory map, CPU bring-up, timer calibration — without touching the VGA
text. The PIT keeps wall-clock duty in rate-generator mode with a boot
self-test, after fixing QEMU's habit of delivering its ticks on the
cascade line.

v0.7.4: boot via GRUB by default (custom two-stage loader kept as
`make run-native`), no hardcoded RAM cap (bitmap covers the full 4GB
address space), and `free`/memory stats report the RAM actually
detected by the bootloader instead of nonsense.

v0.7.0: TCC self-hosting, control flow, flat filesystem. TCC is experimental
and buggy — report any that you find.

## Contributing

Educational project. Network protocol bugs, driver improvements, and shell
features are all welcome.
