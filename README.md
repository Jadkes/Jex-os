# 🪐 JexOS
**The minimal 32-bit Operating System that actually lives.**

JexOS is a from-scratch, x86 hobby operating system designed to bridge the gap between "toy" kernels and functional Unix-like systems. With the release of **v0.3 Persistence**, JexOS has achieved its most significant milestone: a self-contained development environment where code is written, compiled, and stored permanently on disk.

---

## 🚀 Version 0.3: The Persistence Release
The "Persistence" update transforms JexOS from a transient RAM-based system into a real OS with a permanent memory.

### 💾 Hardware-Level Storage (IDE PIO)
JexOS now features a native **ATA IDE Driver** using Programmed I/O (PIO). It communicates directly with the virtual hard drive, allowing the kernel to read and write sectors to a persistent `jexos.img`.

### 📂 JexFS: Minix-Inspired Filesystem
Inspired by the elegance of Linux 0.11 and Minix, we implemented **JexFS**. 
- **Inodes & Superblocks**: Professional metadata management for files and directories.
- **Bitmaps**: Efficient tracking of free blocks and inodes.
- **Persistence**: Every file you create in the shell or edit in Vix survives a reboot.
- **RootFS Packing**: A dedicated host-side tool (`mkjexfs`) allows you to pre-load files into the OS during the build process via the `rootfs/` directory.

### 🛠️ Vix 2.0: The IDE Experience
The built-in text editor is no longer just a notepad; it's a functional IDE:
- **Syntax Highlighting**: Real-time coloring for C keywords, syscalls, functions, and strings.
- **Integrated Build (Ctrl+B)**: One keystroke to save, compile with TCC, and execute.
- **Visual Debugging**: Red indicators for missing semicolons and a detailed register dump on kernel panics.

---

## ✨ Core Features

| Feature | Description |
| :--- | :--- |
| **Self-Hosting** | Compile C code natively using the integrated Tiny C Compiler (TCC). |
| **User Mode** | Stable Ring 3 transition with professional System Call handling (`int 0x80`). |
| **Memory** | Bitmap-based PMM and Identity Mapping Paging (512MB). |
| **VGA Interface** | Custom terminal with hardware cursor, scrolling, and ASCII branding. |
| **Shell** | Unix-style prompt with Command History, inline editing, and binary execution. |
| **Persistence** | Files stored on a 1.44MB HDD image (`jexos.img`). |

---

## 🏔️ The Peak Development Workflow

Experience the ultimate JexOS developer loop:

1. **Host-Side Pre-load**: Drop `.c` files into the `rootfs/` folder on your Linux machine.
2. **Build**: Run `make` to compile the kernel and pack the disk image.
3. **Edit**: Open your code in JexOS: `vix mycode.c`.
4. **Compile & Run**: Press `Ctrl+B` or use the shell:
   ```bash
   root@jexos:/> cc mycode.c -o my_app
   root@jexos:/> ./my_app
   ```
5. **Persistence**: Your binaries and source files are saved to the disk instantly.

---

## 🛠️ Installation & Building

### 1. Prerequisites
Ensure you have the standard x86 build tools and QEMU installed:
```bash
sudo apt update && sudo apt install -y build-essential qemu-system-x86 git
```

### 2. Quick Start
```bash
git clone https://github.com/inasj-cyber/Jex-os.git
cd Jex-os
make      # Builds the kernel, tools, and jexos.img
make run  # Boots JexOS in QEMU
```

---

## 🗺️ Roadmap to v0.4
- [ ] **Multitasking**: Introduction of a scheduler and process forking.
- [ ] **Virtual Memory**: Moving beyond identity mapping to proper process isolation.
- [ ] **Standard C Library Expansion**: Implementing more of `stdlib.h` and `unistd.h`.
- [ ] **Dynamic VFS**: Support for multiple concurrent filesystems.

---

## 🤝 Contributing
JexOS is an educational project. Whether you're fixing a bug in the IDE driver or improving the syntax highlighter, your PRs are welcome!

**Developed with ❤️ for the OS Dev community.**