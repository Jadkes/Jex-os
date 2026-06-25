# JexOS
**The minimal 32-bit hobby OS that actually does networking.**

JexOS is a from-scratch, x86 hobby operating system with a full network stack, multitasking, a persistent filesystem, and a growing suite of developer tools. Born from a "wait, it can do *what*?" approach to kernel development.

---

## Current State: v0.7.0 — TCC Self-Hosting, Control Flow, Flat Filesystem

JexOS can now compile and run non-trivial C programs with control flow, complete with
a fully self-hosting TCC that supports if/else, while, for loops, and hex output.
The expression parser bugs that inverted every comparison and arithmetic operation
have been eliminated, and the filesystem is now flat and uses proper POSIX errno codes.

### TCC Compiler Improvements
- **Control Flow**: `if`/`else`, `while`, `for` — full brace-depth tracking with
  single-pass code generation
- **Hex Printing**: `printf("%x", val)` via new `SYS_PRINT_HEX` syscall (18)
- **New Operators**: `!` (logical not), `&` (address-of), `|`/`||`, `~` (bitwise not)
- **Expression Parser Fixes**: All comparison operators (`>`, `<`, `>=`, `<=`, `==`,
  `!=`), subtraction, division, and modulo now produce correct results — the ModRM
  operand bytes were encoding `right op left` instead of `left op right`
  - **Still experimental**: You can find a lot of bugs on Jexos TCC, if you have find any bugs
  please contact me

### Flat Filesystem
- **Root-Only Layout**: No more `/home/user` — everything lives under `/`
- **Proper Errno Codes**: All kernel syscalls return `-ENOMEM`, `-ENOENT`, `-EINVAL`,
  `-ECHILD`, `-EIO`, `-EPERM`, `-ESRCH` instead of bare `-1`

### TCP Hardening (v0.6.5)
- **Passive Open**: `tcp_listen()` / `tcp_accept()` API for server workloads
- **TCP States**: LISTEN (8), SYN_RCVD (9) — full state machine for passive open
- **MSS Option**: Advertised on SYN for proper segmentation
- **Warnings Cleaned**: Zero compiler warnings across the TCP stack
- **Checksum on All Segments**: TCP checksum computed for every sent segment
- **Connection Tuple Filtering**: Source IP/port matching guards in LISTEN mode

### Memory Management
- **Slab Allocator**: Power-of-2 size classes (16B–2048B) with O(1) `kfree`
- **PMM Fallback**: Handles missing multiboot memory map (QEMU `-kernel` boot)
- **Heap Inspection**: `heapcheck` shows slab committed/free stats

### Process Control
- **Signal Handling**: `sys_signal()` / `sys_kill()` syscalls with 32 signal slots
- **Signal Delivery**: Pending signal check in `task_switch()` scheduler
- **`kill` Command**: `kill <pid>` (SIGTERM), `kill -9 <pid>` (SIGKILL), `kill -l`

### Shell
- **Keyboard Ring Buffer**: ISR stores scancodes — no more dropped keys under load
- **Paged Help**: `more`-style pagination for large command lists
- **Ctrl+L**: Clear screen shortcut
- **Tab Completion**: Commands and filenames
- **Persistent Command History**: Survives reboots (`.history`)
- **Dynamic Prompt**: Color-coded, directory-aware (`root@jexos:/path> `)
- **Process Management**: `ps`, `kill`, `uptime`, `top` commands
- **Editor**: Vix 3.0 IDE with syntax highlighting and `Ctrl+B` build

### Networking
- **RTL8139 NIC Driver**: DMA-based ring buffer, interrupt-driven RX/TX, auto-probe via PCI
- **Full Protocol Stack**: ARP, IP, ICMP (ping), UDP, TCP — all from scratch
- **HTTP Server**: `serve` command — TCP passive open + HTTP/1.0 response
- **HTTP Client**: `fetch` command — TCP GET to retrieve web pages
- **DHCP Client**: `dhcp` command — automatic IP/Gateway/DNS configuration
- **DNS Resolver**: Resolves hostnames with retry loop and fallback polling
- **Gateway Routing**: Automatically routes non-local traffic through gateway
- **Diagnostics**: `ping`, `dhcp`, `arp`, `tcpdump`, `netlog`, `nicregs` commands

### Debug Infrastructure
- **Panic Handler**: Full crash screen with register dump, stack trace, and page fault decode
- **Kernel Ring Buffer** (`dmesg`): Structured event logging with severity levels
- **Stack Unwinder**: Symbolic backtraces via kallsyms
- **ASSERT Macro**: Non-fatal and fatal assertion checking
- **Test Framework**: `runtests` for in-kernel unit tests
- **System Inspection**: `bt` (live backtrace), `dump` (hexdump memory), `heapcheck`, `stackcheck`
- **ftrace-lite**: Lightweight dynamic function tracer
- **GDB Stub**: Remote debugging over serial
- **WARN_ON / BUG_ON**: Kernel warning infrastructure
- **Lockdep**: Lock ordering validator

### Storage & Filesystem
- **JexFS**: Custom persistent filesystem on 1.44MB floppy image
- **IDE PIO Driver**: Low-level ATA disk access
- **Standard Tools**: `cp`, `mv`, `rm`, `ls`, `cat`, `touch`, `mkdir`

---

##  Core Features at a Glance

| Feature | Description |
| :--- | :--- |
| **TCC Control Flow** | `if/else`, `while`, `for` with single-pass code gen |
| **TCC Hex Print** | `printf("%x")` via `SYS_PRINT_HEX` syscall |
| **TCC Operator Fix** | Comparisons, subtraction, division now produce correct results |
| **Flat Filesystem** | Root-only layout, no more `/home/user` |
| **Proper Errno** | `-ENOMEM`, `-ENOENT`, `-EIO`, `-ECHILD` everywhere, not bare `-1` |
| **HTTP Server** | `serve` command — TCP passive open, HTTP/1.0, JexFS-backed |
| **DHCP Client** | DORA cycle, auto-configures IP/Gateway/DNS |
| **TCP Hardened** | Full passive open, proper state machine, clean warnings |
| **Slab Allocator** | Power-of-2 size classes, O(1) kfree, PMM fallback |
| **Full Network Stack** | ARP/IP/ICMP/UDP/TCP from scratch |
| **RTL8139 Driver** | DMA ring buffer, interrupt-driven |
| **Signal Handling** | sys_signal/sys_kill, 32 signal slots |
| **Keyboard Buffer** | Ring buffer, no dropped keys under load |
| **Panic Handler** | Crash screen + stack trace + register dump |
| **Tab Completion** | Commands and filenames |
| **Persistence** | Files and history survive reboots |
| **Multitasking** | Round-robin scheduler with syscalls |
| **Memory Isolation** | Private page directories per process |
| **IDE (Vix 3.0)** | Syntax highlighting + build triggers |

---

## ️Roadmap

- [x] **Slab Allocator**: Power-of-2 size classes, O(1) kfree
- [x] **Keyboard Buffering**: Ring buffer, no dropped keys under load
- [x] **Signal Handling**: sys_signal/sys_kill, delivery in scheduler
- [x] **Ctrl+L**: Clear screen shortcut
- [x] **HTTP Server**: `serve` command — TCP listen/accept + HTTP/1.0
- [x] **DHCP Client**: DORA cycle — automatic IP configuration
- [x] **TCP Hardening**: Passive open, states, checksum, clean warnings
- [x] **TCC Control Flow**: if/else, while, for loops
- [x] **TCC Hex Print**: printf "%x" via SYS_PRINT_HEX
- [x] **TCC Operators**: == != > < >= <= ! ~ & | &&
- [x] **Expression Parser Fix**: Comparisons and arithmetic produce correct results
- [x] **Flat Filesystem**: No /home/user, root-only layout
- [x] **Proper Errno Codes**: -ENOMEM, -ENOENT, -EIO everywhere
- [ ] **JexSnake**: Terminal game
- [ ] **Pipes & Redirection**: Shell IPC (`|`, `>`, `<`)
- [ ] **SMP / Multi-core**: Because one CPU is boring

---

## Building & Running

```bash
make          # Build kernel + disk image
make run      # Launch in QEMU
```

Requires: `gcc` (i386 cross), `nasm`, `qemu-system-i386`.

---

## Contributing

JexOS is an educational project. Network protocol bugs, driver improvements, and shell features are all welcome.

