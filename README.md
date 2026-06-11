# 🪐 JexOS
**The minimal 32-bit hobby OS that actually does networking.**

JexOS is a from-scratch, x86 hobby operating system with a full network stack, multitasking, a persistent filesystem, and a growing suite of developer tools. Born from a "wait, it can do *what*?" approach to kernel development.

---

## 🚀 Current State: Network Stack + Debug Suite

The OS has grown far beyond a basic toy kernel. It now boots on real hardware, talks to the network, runs user-mode processes, and has a comprehensive debug infrastructure.

### 🌐 Networking
- **RTL8139 NIC Driver**: DMA-based ring buffer, interrupt-driven RX/TX, auto-probe via PCI
- **Full Protocol Stack**: ARP, IP, ICMP (ping), UDP, TCP — all from scratch
- **DNS Resolver**: Resolves hostnames with retry loop and fallback polling
- **HTTP Client**: `fetch` command — TCP GET to retrieve web pages
- **Gateway Routing**: Automatically routes non-local traffic through 10.0.2.2
- **Diagnostics**: `ping`, `arp`, `tcpdump`, `netlog`, `nicregs` commands

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

### ⌨️ Shell & UX
- **Tab Completion**: Commands and filenames
- **Persistent Command History**: Survives reboots (`.history`)
- **Dynamic Prompt**: Color-coded, directory-aware (`root@jexos:/path> `)
- **Process Management**: `ps`, `kill`, `uptime`, `top` commands
- **Editor**: Vix 3.0 IDE with syntax highlighting and `Ctrl+B` build

### 📂 Storage & Filesystem
- **JexFS**: Custom persistent filesystem on 1.44MB floppy image
- **VFS Layer**: Mount point dispatch (JexFS, devtmpfs, sysfs)
- **IDE PIO Driver**: Low-level ATA disk access
- **Standard Tools**: `cp`, `mv`, `rm`, `ls`, `cat`, `touch`, `mkdir`

---

## ✨ Core Features at a Glance

| Feature | Description |
| :--- | :--- |
| **Full Network Stack** | ARP/IP/ICMP/UDP/TCP from scratch |
| **RTL8139 Driver** | DMA ring buffer, interrupt-driven |
| **Panic Handler** | Crash screen + stack trace + register dump |
| **Tab Completion** | Commands and filenames |
| **Persistence** | Files and history survive reboots |
| **Multitasking** | Round-robin scheduler with syscalls |
| **Memory Isolation** | Private page directories per process |
| **IDE (Vix 3.0)** | Syntax highlighting + build triggers |

---

## 🗺️ Roadmap

- [ ] **JexSnake**: Terminal game
- [ ] **Pipes & Redirection**: Shell IPC (`|`, `>`, `<`)
- [ ] **DHCP Client**: Automatic IP configuration
- [ ] **Stable TCP**: Full connection lifecycle, windowing
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
