# JexOS Roadmap

The OS has evolved beyond the original v0.5 goals. Here's what's next.

## ✅ Completed Milestones

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
- [x] **Persistent Command History**: `.history` survives reboots
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
- [x] **JexFS**: Persistent filesystem with hierarchy
- [x] **VFS Layer**: Mount point dispatch
- [x] **devtmpfs/sysfs**: Virtual filesystems
- [x] **IDE PIO Driver**: ATA disk access
- [x] **Standard commands**: `cp`, `mv`, `rm`, `ls`, `cat`, `mkdir`, `cd`

### Kernel Infrastructure
- [x] **Initcall Framework**: Automatic driver/subsystem initialization
- [x] **Multitasking**: Round-robin scheduler
- [x] **Memory Isolation**: Private page directories per process
- [x] **Stack Guard Pages**: Overflow detection
- [x] **kallsyms**: Symbol table for backtraces
- [x] **vsnprintf/snprintf**: Safe formatted output
- [x] **Workqueue**: Deferred execution

---

## 🎯 Current Goals

### SMP Follow-ups
- [ ] **Spinlocks / Mutexes**: shared kernel structures (ARP cache, TCP tables, kheap) are currently protected by global IF games only
- [ ] **Load Balancing**: APs sit idle in hlt; push ready tasks to the second CPU
- [ ] **IPI Reschedule**: wake a CPU's scheduler remotely instead of polling
- [ ] **LAPIC Timer**: per-CPU tick source, drop the PIT cascade workaround
- [ ] **Atomic Console Writes**: serial lines from BSP and AP interleave mid-line

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
- [ ] **Stack Canaries per Task**: guard pages exist; canaries complete the set
- [ ] **Screensaver**: starfield in text mode with block glyphs. Pure fun, zero risk

### Polish
- [x] **Ctrl+L**: Clear screen shortcut
- [x] **Music Tempo**: play_tune rescored for the now-correct clock
- [ ] **Scrollback**: Terminal history buffer
- [ ] **Color Themes**: Customizable palette
