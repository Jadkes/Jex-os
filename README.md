# 🪐 JexOS
**The minimal 32-bit Operating System that actually lives.**

JexOS is a from-scratch, x86 hobby operating system designed to bridge the gap between "toy" kernels and functional Unix-like systems. With the release of **v0.4.1**, JexOS has entered the **Multitasking Era**, supporting concurrent processes and a real-time scheduler.

---

## 🚀 Version 0.4.1: The Multitasking Release
This milestone transforms JexOS from a single-tasking kernel into a true multitasking environment.

### 👥 Process Management & Scheduling
- **Round-Robin Scheduler**: A timer-driven (IRQ0) scheduler that automatically switches between processes every 10ms.
- **Process Control Blocks (PCB)**: A professional `task_t` structure (inspired by Linux 0.11 `task_struct`) to manage CPU state, PIDs, and stacks.
- **`fork()` Capability**: The OS can now clone the current process. Try it in the shell to see the parent and child running simultaneously!
- **`ps` Utility**: A new system command to list all active processes and their current states (READY, RUNNING, ZOMBIE).

### ✍️ Vix 3.0: The IDE Experience
- **Infinite Vertical Scrolling**: Edit large source files with automatic view tracking.
- **Advanced Syntax Highlighting**: Real-time coloring for C keywords, numbers, comments, and syscalls.
- **Integrated Build**: Press `Ctrl+B` to save, compile, and run your code without leaving the editor.

### 📂 JexFS Persistence
- **Hardware IDE Driver**: Communicates directly with the `jexos.img` via PIO.
- **Unix-style Utilities**: `mkdir`, `cp`, `rm`, `mv`, and `cd` support for a full hierarchical filesystem.

---

## ✨ Core Features

| Feature | Description |
| :--- | :--- |
| **Multitasking** | Concurrent process execution with a preemptive scheduler. |
| **Self-Hosting** | Compile C code natively using the integrated Tiny C Compiler (TCC). |
| **User Mode** | Stable Ring 3 transition with professional System Call handling (`int 0x80`). |
| **IDE (Vix 3.0)** | Professional code editing with scrolling and advanced highlighting. |
| **Persistence** | Real hardware storage on a 1.44MB HDD image. |

---

## 🏔️ The Peak Development Workflow

1. **Create & Edit**: `vix hello.c`
2. **Build & Run**: Press `Ctrl+B` inside the IDE.
3. **Multitask**: Use `fork` in the shell to spawn background tasks.
4. **Monitor**: Use `ps` to see your tasks in action.

---

## 🛠️ Installation & Building

```bash
git clone https://github.com/inasj-cyber/Jex-os.git
cd Jex-os
make      # Builds kernel, tools, and persistent image
make run  # Boots JexOS in QEMU
```

---

## 🗺️ Roadmap to v0.5
- [ ] **Virtual Memory Isolation**: Individual address spaces for every process.
- [ ] **Pipes & Redirection**: Standard IPC support for the shell.
- [ ] **Signal Handling**: Implementing `signal.h` for process control.

---

## 🤝 Contributing
JexOS is an educational project. Contributions to the kernel, scheduler optimization, or driver features are welcome!

**Developed with ❤️ for the OS Dev community.**
