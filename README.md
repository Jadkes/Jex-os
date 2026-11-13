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
- **Filesystem** — JexFS on a 64 MB disk image, mounted directly from the drive (the GRUB module copy is released to the PMM at boot); hierarchy, proper errno codes; persistence via `make deliver` (survives reboots from read-only media)
- **Audio** — AC'97 bus-master PCM at 44.1 kHz/16-bit stereo, and `play` for MP3 files decoded straight out of JexFS; MP3s dropped in `rootfs/` are transcoded to the codec's format at image-build time
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

The disk image is 64 MB (`IMG_BLOCKS=65536` 1 KB blocks), the largest
JexFS can address: inode block numbers are 16-bit and the block bitmap is
8 blocks. GRUB still loads it as a multiboot module so a diskless boot
works, but the kernel prefers the disk: it validates a superblock at LBA 0
(or at the tail of a `make deliver` image), switches block I/O there, and
hands the module's pages back to the allocator. On a 128 MB guest that
takes free memory from 61,708 KB to 127,232 KB. `make run` boots a
throwaway `build/run-jexos.img` copy so guest writes do not dirty the
build artifact.

Anything in `rootfs/` is staged into the image at build time. `*.mp3` in
`rootfs/` is re-encoded to 44.1 kHz, 96 kbit/s (the only rate the kernel
decoder accepts) without touching the originals; override with
`make ROOTFS_MP3_BITRATE=128`. Other files are copied through. If ffmpeg
is missing or fails on a file, the original is packed with a warning
rather than failing the build.

Requires `gcc` (i386), `binutils`, `python3`, `grub2-tools`, and
`qemu-system-i386`; `ffmpeg` for build-time MP3 transcoding (optional).

## Roadmap

- [x] Self-hosting TCC
- [x] TCP server (passive open)
- [x] SMP scheduling on all CPUs (spinlocks, per-CPU timers, load spreading)
- [x] JexFS persistence: `make deliver` builds a hybrid ISO with rootfs appended at the tail; the kernel mounts that tail directly — history and files survive reboots from read-only media
- [x] Disk-first rootfs: mount the on-disk copy and return the 64 MB GRUB module to the PMM (61,708 → 127,232 KB free on a 128 MB guest)
- [ ] JexFS v3: 32-bit block numbers + 4 KB blocks, extent tree, metadata journal + ordered data, crc32c metadata checksums, `mkjexfs3`/`jexfsck`, v2→v3 converter (Phase 0 done)
- [x] TLB shootdown IPIs: `free_kernel_stack()` flushes remote TLBs via IPI before returning frames to PMM
- [x] 64 MB disk image: `IMG_BLOCKS=65536` blocks, the maximum JexFS can address with 16-bit block numbers
- [x] Audio: AC'97 44.1 kHz stereo PCM, MP3 playback from JexFS, and build-time MP3 transcoding for `rootfs/`
- [ ] PID 2 boot stack page: give the BSP task a proper kernel stack page so ISR frames never land on the real boot stack (eliminates SMP iret corruption class)
- [x] Per-task FPU state: eager `FXSAVE`/`FXRSTOR` per task (plain `FNSAVE`/`FRSTOR` without FXSR), wired into `task_switch()` and AP bring-up; the kernel still builds `-mno-80387` and only `drivers/mp3.o` opts back in. Ring-3 float is not exercised by the suite and there is no lazy `#NM`/`CR0.TS` path yet
- [ ] Pipes & redirection

## Status

v0.8.0: bigger disk, the RAM back, and working audio. The JexFS image is
now 64 MB — the v2 format's ceiling, since inode block numbers are 16-bit
and the block bitmap is 8 blocks. GRUB still loads it as a module, but the
kernel no longer keeps that copy alive: it validates a JexFS superblock at
LBA 0 or at the delivery tail, mounts the disk copy, and returns the
module's 64 MB to the PMM. On a 128 MB guest free memory goes from
61,708 KB at PMM init to 127,232 KB after the switch, and `make run`
boots a throwaway copy so the build artifact stays clean. The image rule
also stages `rootfs/` and re-encodes every MP3 to 44.1 kHz / 96 kbit/s at
build time, which is the only rate the in-kernel decoder accepts; the
originals are never modified and `ROOTFS_MP3_BITRATE` overrides the rate.

Audio was fixed rather than just added. AC'97 playback alternated between
music and silence because CIV/LVI are 5-bit indices that wrap at 32 while
the driver published an 8-entry descriptor list: past the first cycle the
bus master read descriptors 8–31 out of bounds and halted at LVI, and
with `out.mixing-engine=off` the backend padded the stall with silence.
The list is 32 entries now, LVI is an absolute index, and only
descriptors the ring can fill are queued. Measured on the same QEMU path
that exposed the bug: a 4 s tone records as one continuous block and the
song plays without gaps.

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
ISO; booted as an IDE hard disk, the kernel identifies the drive and
mounts that tail as the live rootfs. Writes go straight there, so history
and files survive reboots from read-only media without a mirror copy.

JexFS v3 is underway. Phase 0 (disk-first mount) is in; the format work is
a real break: 32-bit block numbers and 4 KB blocks, an extent tree,
a metadata journal with ordered data, and crc32c checksums, with
`mkjexfs3`, a host verifier and a v2→v3 converter. See `TODO.md` for the
phase table.

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
