# 🪐 JexOS
**The minimal 32-bit Operating System that actually lives.**

JexOS is a from-scratch, x86 hobby operating system designed to bridge the gap between "toy" kernels and functional Unix-like systems. With the release of **v0.3.4**, JexOS has achieved its most fluid user experience yet, featuring dynamic navigation and a robust set of Unix-standard filesystem utilities.

---

## 🚀 Version 0.3.4: The Navigation & Utilities Update
The "Navigation" update transforms JexOS from a flat storage system into a hierarchical, interactive environment mirroring the feel of a classic Linux terminal.

### 🗺️ Dynamic Linux-style Navigation
- **Active Path Prompt**: The shell now displays your current working directory in real-time (e.g., `root@jexos:/home/jad> `).
- **Interactive `cd`**: Full support for directory traversal, including `cd ..` to move up and `cd` to return to root.
- **Path-Aware `ls`**: List contents of the current directory or specify a target path (e.g., `ls /bin`).

### 📂 Advanced JexFS Utilities
We've expanded the JexFS (Minix-inspired) toolkit to support a full development workflow:
- **`mkdir`**: Create nested directory structures.
- **`cp`**: Copy files between directories with reliable multi-block streaming.
- **`rm`**: Safely delete files and free up disk space via bitmap management.
- **`mv`**: Rename files instantly with metadata-only updates.
- **Clean Listings**: Automatic hiding of `.` and `..` system entries for a cleaner `ls` output.

### 🛠️ Peak Compiler Upgrades
- **Custom Output Names**: Use the `-o` flag with `cc` to name your binaries (e.g., `cc hello.c -o peak`).
- **Stable Binary Execution**: Fixed filesystem seek and write bugs to ensure compiled ELF binaries execute flawlessly every time.

---

## ✨ Core Features

| Feature | Description |
| :--- | :--- |
| **Self-Hosting** | Compile C code natively using the integrated Tiny C Compiler (TCC). |
| **User Mode** | Stable Ring 3 transition with professional System Call handling (`int 0x80`). |
| **IDE (Vix 2.0)** | Syntax highlighting, line numbers, and integrated `Ctrl+B` build triggers. |
| **Persistence** | Real hardware communication via IDE PIO driver to `jexos.img`. |
| **Unix Feel** | Command history, adaptive prompt, and standard utility suite. |

---

## 🏔️ The Peak Development Workflow

1. **Host-Side Pre-load**: Drop files into `rootfs/` on your host machine.
2. **Build & Pack**: Run `make` to generate the persistent `jexos.img`.
3. **Navigate & Organize**:
   ```bash
   root@jexos:/> mkdir dev
   root@jexos:/> mv hello.c dev/
   root@jexos:/> cd dev
   ```
4. **Build & Run**: Use Vix 2.0 or the shell:
   ```bash
   root@jexos:/dev> cc hello.c -o my_app
   root@jexos:/dev> ./my_app
   ```

---

## 🛠️ Installation & Building

```bash
git clone https://github.com/inasj-cyber/Jex-os.git
cd Jex-os
make      # Builds kernel and persistent image
make run  # Boots JexOS in QEMU
```

---

## 🗺️ Roadmap to v0.4
- [ ] **Multitasking**: Scheduler and process management.
- [ ] **Virtual Memory**: Process isolation and paging.
- [ ] **Pipes & Redirection**: Standard IPC support.

---

## 🤝 Contributing
JexOS is an educational project. Contributions to the kernel, driver optimization, or filesystem features are welcome!

**Developed with ❤️ for the OS Dev community.**
