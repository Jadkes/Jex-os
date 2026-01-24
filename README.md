# 🪐 JexOS
**The minimal 32-bit Operating System that actually lives.**

JexOS is a from-scratch, x86 hobby operating system designed to bridge the gap between "toy" kernels and functional Unix-like systems. With the release of **v0.3.8**, JexOS introduces a significantly more powerful development environment with the all-new Vix 3.0 IDE.

---

## 🚀 Version 0.3.8: The IDE Power-Up
This release focuses on making the built-in development tools professional-grade, enabling the editing of large source files and providing deeper code insights.

### ✍️ Vix 3.0: A Real IDE Experience
The Vix editor has been completely overhauled to support professional coding workflows:
- **Infinite Vertical Scrolling**: Edit large C files that span multiple pages. The editor now tracks your cursor and scrolls the view automatically.
- **Enhanced Syntax Highlighting**:
  - **Keywords**: `int`, `void`, `struct`, `static`, etc. (**Cyan**)
  - **Numbers**: Constant values and literals (**Yellow**)
  - **Comments**: Full support for `// single-line comments` (**Gray**)
  - **Standard Library**: Functions like `malloc`, `free`, `open`, `read` are now recognized (**Green**)
  - **Braces & Symbols**: `{}`, `()`, `[]` and operators (**Orange**)
- **State Awareness**: A new `[MODIFIED]` indicator in the status bar warns you of unsaved changes.
- **Robustness**: Improved backspace logic for line merging and protection against accidental control character insertion.

### 🗺️ Navigation & Shell Improvements
- **Refined Prompt**: A tighter, Linux-accurate prompt: `root@jexos:/path> `.
- **Fixed Path Logic**: Solidified `cd ..` and `cd` behavior for reliable directory traversal.
- **Large File Support**: Fixed JexFS write/read buffers to ensure large compiled binaries (like those produced by TCC) are stored without corruption.

---

## ✨ Core Features

| Feature | Description |
| :--- | :--- |
| **Self-Hosting** | Compile C code natively using the integrated Tiny C Compiler (TCC). |
| **User Mode** | Stable Ring 3 transition with professional System Call handling (`int 0x80`). |
| **IDE (Vix 3.0)** | Infinite scrolling, advanced highlighting, and integrated `Ctrl+B` build. |
| **Persistence** | Real hardware communication via IDE PIO driver to `jexos.img`. |
| **Unix Feel** | Dynamic path-aware prompt, standard utilities (`cp`, `mv`, `rm`, `mkdir`). |

---

## 🏔️ The Peak Development Workflow

1. **Host-Side Pre-load**: Drop files into `rootfs/` on your host machine.
2. **Build & Pack**: Run `make` to generate the persistent `jexos.img`.
3. **Navigate & Organize**:
   ```bash
   root@jexos:/> mkdir dev
   root@jexos:/> cd dev
   ```
4. **Edit & Build**: Open code in Vix 3.0, write your logic, and press `Ctrl+B`:
   ```bash
   root@jexos:/dev> vix main.c
   # Inside Vix: Press Ctrl+B to Save, Compile, and Run instantly!
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