# 🪐 JexOS
**The minimal 32-bit Operating System that actually lives.**

JexOS is a from-scratch, x86 hobby operating system designed to bridge the gap between "toy" kernels and functional Unix-like systems. With the release of **v0.5.1**, JexOS enters the "Peak UX" phase, offering a professional terminal experience with modern features.

---

## 🚀 Version 0.5.1: The Peak UX Update
This release focuses on making JexOS a joy to use, bringing standard Linux terminal quality-of-life features to the shell.

### ⌨️ Professional Shell UX
- **Tab Completion**: Intelligent autocompletion for commands and filenames. Press `TAB` to speed up your workflow.
- **Persistent Command History**: Your shell history is now stored on disk (`.history`). Commands survive reboots, just like a real Unix system.
- **Dynamic Linux Prompt**: An adaptive, color-coded prompt that tracks your current directory: `root@jexos:/path> `.
- **Host-to-Guest Copy/Paste**: Optimized serial bridge for instant code injection from your host terminal.

### ✍️ Vix 3.0: The IDE Experience
- **Infinite Vertical Scrolling**: Edit large source files with automatic view tracking.
- **Advanced Syntax Highlighting**: Real-time coloring for C keywords, numbers, comments, and syscalls.
- **Integrated Build**: Press `Ctrl+B` to save, compile, and run your code instantly.

### 📂 JexFS Persistence
- **Hierarchy Support**: Full support for nested directories and navigation (`cd`, `mkdir`).
- **Standard Utilities**: A suite of professional-grade tools: `cp`, `mv`, `rm`, `ls`, `cat`, and `touch`.
- **Reliable Storage**: Powered by a native IDE PIO driver communicating with a persistent `jexos.img`.

---

## ✨ Core Features

| Feature | Description |
| :--- | :--- |
| **Tab Completion**| Press TAB to autocomplete commands and files. |
| **Persistence** | Permanent file and history storage on disk. |
| **Self-Hosting** | Native C compilation using the integrated TCC. |
| **Memory Isolation**| Private page directories for every process. |
| **IDE (Vix 3.0)** | Professional code editor with build triggers. |

---

## 🗺️ Roadmap to v0.6
- [ ] **JexSnake**: The first official terminal game.
- [ ] **Multitasking Scheduler**: Preemptive context switching (Stable).
- [ ] **Pipes & Redirection**: Standard IPC support.

---

## 🤝 Contributing
JexOS is an educational project. Contributions to process management, driver optimization, or UX features are welcome!

**Developed with ❤️ for the OS Dev community.**
