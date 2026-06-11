# 🪐 JexOS
**The minimal 32-bit hobby OS that actually does networking.**

JexOS is a from-scratch, x86 hobby operating system with a full network stack, multitasking, a persistent filesystem, and a growing suite of developer tools. Born from a "wait, it can do *what*?" approach to kernel development.

---

## 🚀 Current State: v0.6.5 — TCP Hardening, HTTP Server, DHCP Client

JexOS can now serve web pages, auto-configure its network, and handle full TCP connection lifecycles. What started as a "can it ping?" experiment has grown into an OS that runs its own HTTP server.

### 🌐 HTTP Server
- **`serve` Command**: Listens on port 80, accepts TCP connections, serves HTTP/1.0
- **JexFS File Serving**: Serves files from the filesystem (`/index.html`, etc.)
- **Default Page**: Falls back to a "Hello from JexOS!" page when no file found
- **Passive TCP Open**: Full LISTEN → SYN_RCVD → ESTABLISHED three-way handshake
- **Content-Type/Length**: Proper HTTP headers with content negotiation

### 🔄 DHCP Client
- **DORA Cycle**: Discover → Offer → Request → Ack over UDP broadcast
- **Auto-Configuration**: Sets IP, gateway, and DNS server at runtime
- **Runtime IP Variables**: `OUR_IP`, `GATEWAY_IP`, `DNS_SERVER` are mutable globals
- **`dhcp` Command**: One-command network setup
- **QEMU Slirp Compatible**: Works out of the box with QEMU's built-in DHCP server
- **Retry Logic**: 3 retries with timeout per message

### 🔧 TCP Hardening
- **Passive Open**: `tcp_listen()` / `tcp_accept()` API for server workloads
- **TCP States**: LISTEN (8), SYN_RCVD (9) — full state machine for passive open
- **MSS Option**: Advertised on SYN for proper segmentation
- **Warnings Cleaned**: Zero compiler warnings across the TCP stack
- **Checksum on All Segments**: TCP checksum computed for every sent segment
- **Connection Tuple Filtering**: Source IP/port matching guards in LISTEN mode

### 🧠 Memory Management
- **Slab Allocator**: Power-of-2 size classes (16B–2048B) with O(1) `kfree`
- **PMM Fallback**: Handles missing multiboot memory map (QEMU `-kernel` boot)
- **Heap Inspection**: `heapcheck` shows slab committed/free stats

### ⚡ Process Control
- **Signal Handling**: `sys_signal()` / `sys_kill()` syscalls with 32 signal slots
- **Signal Delivery**: Pending signal check in `task_switch()` scheduler
- **`kill` Command**: `kill <pid>` (SIGTERM), `kill -9 <pid>` (SIGKILL), `kill -l`

### ⌨️ Shell & UX
- **Keyboard Ring Buffer**: ISR stores scancodes — no more dropped keys under load
- **Paged Help**: `more`-style pagination for large command lists
- **Ctrl+L**: Clear screen shortcut
- **Tab Completion**: Commands and filenames
- **Persistent Command History**: Survives reboots (`.history`)
- **Dynamic Prompt**: Color-coded, directory-aware (`root@jexos:/path> `)
- **Process Management**: `ps`, `kill`, `uptime`, `top` commands
- **Editor**: Vix 3.0 IDE with syntax highlighting and `Ctrl+B` build

### 🌐 Networking
- **RTL8139 NIC Driver**: DMA-based ring buffer, interrupt-driven RX/TX, auto-probe via PCI
- **Full Protocol Stack**: ARP, IP, ICMP (ping), UDP, TCP — all from scratch
- **HTTP Server**: `serve` command — TCP passive open + HTTP/1.0 response
- **HTTP Client**: `fetch` command — TCP GET to retrieve web pages
- **DHCP Client**: `dhcp` command — automatic IP/Gateway/DNS configuration
- **DNS Resolver**: Resolves hostnames with retry loop and fallback polling
- **Gateway Routing**: Automatically routes non-local traffic through gateway
- **Diagnostics**: `ping`, `dhcp`, `arp`, `tcpdump`, `netlog`, `nicregs` commands

### 🐛 Debug Infrastructure
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

### 📂 Storage & Filesystem
- **JexFS**: Custom persistent filesystem on 1.44MB floppy image
- **VFS Layer**: Mount point dispatch (JexFS, devtmpfs, sysfs)
- **IDE PIO Driver**: Low-level ATA disk access
- **Standard Tools**: `cp`, `mv`, `rm`, `ls`, `cat`, `touch`, `mkdir`

---

## ✨ Core Features at a Glance

| Feature | Description |
| :--- | :--- |
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

## 🗺️ Roadmap

- [x] **Slab Allocator**: Power-of-2 size classes, O(1) kfree
- [x] **Keyboard Buffering**: Ring buffer, no dropped keys under load
- [x] **Signal Handling**: sys_signal/sys_kill, delivery in scheduler
- [x] **Ctrl+L**: Clear screen shortcut
- [x] **HTTP Server**: `serve` command — TCP listen/accept + HTTP/1.0
- [x] **DHCP Client**: DORA cycle — automatic IP configuration
- [x] **TCP Hardening**: Passive open, states, checksum, clean warnings
- [ ] **JexSnake**: Terminal game
- [ ] **Pipes & Redirection**: Shell IPC (`|`, `>`, `<`)
- [ ] **SMP / Multi-core**: Because one CPU is boring

---

## 🏗️ Building & Running

```bash
make          # Build kernel + disk image
make run      # Launch in QEMU
```

Requires: `gcc` (i386 cross), `nasm`, `qemu-system-i386`.

---

## 🤝 Contributing

JexOS is an educational project. Network protocol bugs, driver improvements, and shell features are all welcome.

**Developed with ❤️ for the OS Dev community.**
