# JexOS Roadmap

The OS has evolved beyond the original v0.5 goals. Here's what's next.

## ✅ Completed Milestones

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
- [ ] **Single core optimisation**: Because a lot of cpu's means a lot of bugs, better rely on 1
- [ ] **AHCI / SATA**: Faster disk I/O
- [ ] **USB Keyboard/Mouse**: Beyond PS/2

### Polish
- [x] **Ctrl+L**: Clear screen shortcut
- [ ] **Scrollback**: Terminal history buffer
- [ ] **Color Themes**: Customizable palette
