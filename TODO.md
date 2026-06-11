# JexOS Roadmap

The OS has evolved beyond the original v0.5 goals. Here's what's next.

## ✅ Completed Milestones

### Shell & UX
- [x] **Tab Completion**: Auto-complete filenames and commands
- [x] **Persistent Command History**: `.history` survives reboots
- [x] **Dynamic Prompt**: Color-coded, directory-aware
- [x] **Uptime**: System uptime command
- [x] **Top**: Per-task CPU usage monitor
- [x] **Boot Banner**: Build info + device listing on startup

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

### Networking Hardening
- [ ] **DHCP Client**: Automatic IP configuration
- [ ] **TCP Retransmit**: Handle packet loss gracefully
- [ ] **TCP Connection Close**: Proper FIN handshake
- [ ] **Checksum Validation**: Verify on receive, not just on send
- [ ] **Concurrent Connections**: Multiple sockets

### Shell & Utilities
- [ ] **JexSnake**: Terminal snake game
- [ ] **Pipes & Redirection**: `|`, `>`, `<` operators
- [ ] **Command Aliases**: `alias ll='ls -l'`
- [ ] **Grep**: Text search within files
- [ ] **Find**: File search by name
- [ ] **Environment Variables**: `PATH` support

### System Improvements
- [ ] **Dynamic Memory**: Replace bump allocator with proper slab/free-list
- [ ] **Keyboard Buffering**: Don't drop keys under load
- [ ] **Signal Handling**: Basic Unix signal delivery
- [ ] **SMP / Multi-core**: Because one CPU is boring
- [ ] **AHCI / SATA**: Faster disk I/O
- [ ] **USB Keyboard/Mouse**: Beyond PS/2

### Polish
- [ ] **Ctrl+L**: Clear screen shortcut
- [ ] **Scrollback**: Terminal history buffer
- [ ] **Color Themes**: Customizable palette
