# JexOS

A minimal 32-bit Operating System built for learning purposes.

## 🚀 Version 0.2 Stable Release: The Self-Hosting Peak
Version 0.2 marks a major milestone for JexOS, achieving stable native C development within the operating system. You can now write, compile, and run C programs without ever leaving JexOS.

### ✨ NEW in v0.2: Self-Hosting & IDE Experience
- **Stable Native Compilation**: Full integration of TCC (Tiny C Compiler) with support for standard C syntax including `printf` and escape sequences like `\n`.
- **Vix Editor 2.0**: Upgraded from a simple text box to a functional IDE:
  - **Syntax Highlighting**: Keywords (Cyan), Functions (Yellow), Strings/Parens (Orange), and Syscalls (Green).
  - **Integrated Build (Ctrl+B)**: Save, compile, and execute your code with a single keystroke.
  - **Line Numbering**: Essential for debugging and navigating code.
  - **Logical Cursor & Editing**: Real character insertion and deletion at any position in the buffer.
  - **Save Indicators & Quit Confirmation**: Visual feedback for file operations (`[SAVE OK]`) and protection against unsaved changes.
  - **Auto-Semicolon Check**: Visual red indicator (`/`) for lines missing required semicolons.
- **Robust Memory Management**: 
  - **Enhanced sbrk**: Fixed kernel paging to properly map physical frames to user-space heap requests.
  - **Dynamic Allocation**: Reliable `malloc()` and `free()` support for user applications.
- **Improved ELF Loading**: Fixed program header resolution and memory mapping for stable binary execution.

## Features

- **Multiboot Compliant**: Boots with GRUB or QEMU.
- **32-bit Protected Mode**: Uses GDT, IDT, and TSS.
- **User Mode (Ring 3)**: Full privilege separation with stable system calls (`int 0x80`).
- **Interrupt Handling**: Custom ISRs and IRQs (Keyboard, Timer, RTC).
- **VGA Text Mode**: Scrolling, hardware cursor, and beautiful Cyan/Yellow syntax coloring.
- **Filesystem**: FAT12 RAM-disk with case-preserving filenames and basic file operations.
- **Shell**: `root@jexos:/>` prompt with History, Cursor movement, and native build tools.

## How to Build and Run

### 1. Installation of Prerequisites
```bash
sudo apt update && sudo apt install -y build-essential qemu-system-x86 git
```

### 2. Download the Project
```bash
git clone https://github.com/inasj-cyber/Jex-os.git
cd Jex-os
```

### 3. Building and Running
```bash
make      # Compile the OS
make run  # Launch in QEMU
```

## Self-Hosting Usage Examples

Once in JexOS, try the peak development workflow:

```bash
# 1. Create a C file automatically
root@jexos:/> mkcode

# 2. Open it in the new Vix 2.0 IDE
root@jexos:/> vix hello.c

# 3. Edit your code, then press Ctrl+B to Build & Run!
# Or compile manually via shell:
root@jexos:/> tcc hello.c
```

## Project Structure

- `src/boot.s`: Assembly entry point.
- `src/kernel.c`: Main kernel logic.
- `src/editor.c`: Vix 2.0 IDE (Syntax Highlighting, Line Numbers).
- `src/tcc.c`: Tiny C Compiler integration.
- `src/sbrk.c`: User-mode heap management with paging.
- `src/exec.c`: ELF loader & execution logic.
- `src/fat12.c`: FAT12 Filesystem driver.
- `src/syscall.c`: System Calls (Print, Exit, Malloc, etc).

## Roadmap to v0.3
- [ ] **Persistent Disk Image**: Move from RAM-disk to a real bootable `.img` file.
- [ ] **Advanced File System**: Inspired by Linux 0.11 FS for better file management.
- [ ] **Integrated Image Generator**: `make img` to generate fresh disk images with pre-loaded headers and libraries.

## Disclaimer
This OS is for educational purposes only.
It is not intended for production use.
