# 🪐 JexOS
**The minimal 32-bit Operating System that actually lives.**

JexOS is a from-scratch, x86 hobby operating system designed to bridge the gap between "toy" kernels and functional Unix-like systems. With the release of **v0.4.8**, JexOS introduces the "Lazy Peak" workflow, seamlessly connecting your host development environment with the guest OS.

---

## 🚀 Version 0.4.8: The Lazy Peak (Copy/Paste) Update
This release focuses on developer productivity, making it easier than ever to bring external code into JexOS.

### 📋 Host-to-Guest Copy/Paste
- **High-Speed Serial Bridge**: JexOS now treats the serial port (COM1) as a second keyboard. 
- **Instant Injection**: Simply paste code into your Linux terminal, and it will be "typed" into JexOS instantly. 
- **Universal Input**: Works everywhere—pasting C functions into Vix 3.0 or complex commands into the shell.
- **Optimized Rendering**: The Vix editor is now smart enough to handle high-speed text streams without lagging the CPU.

### 👥 Multitasking & Isolation (v0.4.x Foundations)
- **Virtual Memory Isolation**: Every process now runs in its own private 4GB virtual address space.
- **Deep Page Cloning**: `fork()` now creates full isolated copies of process memory, preventing cross-process crashes.
- **Stable Scheduler**: A round-robin preemptive scheduler managing concurrent task states.

### 📂 Persistence & IDE
- **JexFS**: Permanent storage on a 1.44MB HDD image.
- **Vix 3.0**: A professional IDE with infinite scrolling, syntax highlighting, and integrated build tools.

---

## ✨ Core Features

| Feature | Description |
| :--- | :--- |
| **Copy/Paste** | Inject text from your host OS directly into JexOS applications. |
| **Memory Isolation** | Private page directories for every process (v0.4.4+). |
| **Self-Hosting** | Native C compilation using the integrated TCC. |
| **IDE (Vix 3.0)** | Advanced code editor with real-time feedback and build triggers. |
| **Persistence** | Persistent file storage surviving reboots. |

---

## 🏔️ The Lazy Peak Workflow

1. **Host-Side Copy**: Copy any C code block on your Linux machine.
2. **Open Editor**: In JexOS, type `vix main.c`.
3. **Paste**: Right-click/Paste into your Linux terminal. Watch your code appear instantly in JexOS!
4. **Build & Run**: Press `Ctrl+B` inside Vix to execute your pasted code.

---

## 🛠️ Installation & Building

```bash
git clone https://github.com/inasj-cyber/Jex-os.git
cd Jex-os
make      # Builds kernel and persistent image
make run  # Boots JexOS in QEMU
```

---

## 🤝 Contributing
JexOS is an educational project. Contributions to process management, IPC, or driver optimization are welcome!

**Developed with ❤️ for the OS Dev community.**