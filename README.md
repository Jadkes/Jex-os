# JexOS

A minimal 32-bit Operating System built for learning purposes.

## 🚀 Version 0.1 Final Stable Release
This version represents the culmination of JexOS 0.1 development. It features a fully self-hosting capable foundation with a working text editor (`vix`), binary execution (`exec`), and a robust file system.

## Features

- **Multiboot Compliant**: Boots with GRUB or QEMU.
- **32-bit Protected Mode**: Uses GDT, IDT, and TSS.
- **User Mode (Ring 3)**: Full privilege separation with stable system calls (`int 0x80`).
- **Interrupt Handling**: Custom ISRs and IRQs (Keyboard, Timer, RTC).
- **Keyboard Driver**: Fully functional with backspace, **Shift**, **Arrows** (Navigation), and **Command History**.
- **VGA Text Mode**: Scrolling, newline support, hardware cursor, and Cyan-themed ASCII art.
- **Filesystem**: FAT12 RAM-disk with basic file operations (`ls`, `touch`, `cat`, `echo`, `rm`).
- **Editor**: **Vix** (Visual Editor) - A nano-like text editor running in Kernel Mode.
- **Shell**: `root@jexos:/>` prompt with:
  - **History**: Up/Down arrows to recall commands.
  - **Editing**: Left/Right arrows to move cursor.
  - `help`: List commands.
  - `vix <file>`: Edit files interactively.
  - `ls`, `touch`, `cat`, `echo`, `rm`: File management.
  - `exec <file>`: Run ELF binaries.
  - `mktest`: Create a sample ELF binary.
  - `clear`: Clear the screen.
  - `music`: Play the JexOS fanfare.
  - `reboot`/`shutdown`: Power management.
- **Memory Management**:
  - **PMM**: Physical Memory Manager (Bitmap-based).
  - **Paging**: Identity mapping (first 512MB).
  - **KHeap**: Kernel heap allocator.
- **Kernel Panic**: Custom "Red Screen of Death".

## How to Build and Run

### 1. Installation of Prerequisites

Choose the command for your Linux distribution:

#### **Debian / Ubuntu / Kali (APT)**
```bash
sudo apt update && sudo apt install -y build-essential qemu-system-x86 git
```

#### **Fedora / RHEL / CentOS (DNF)**
```bash
sudo dnf install -y @development-tools qemu-system-x86 git
```

#### **Arch Linux (PACMAN)**
```bash
sudo pacman -S base-devel qemu-desktop git
```

### 2. Download the Project

```bash
git clone https://github.com/inasj-cyber/Jex-os.git
cd Jex-os
```

### 3. Building and Running

```bash
make      # Compile the OS
make run  # Launch in QEMU with Sound enabled
```

## Project Structure

- `src/boot.s`: Assembly entry point.
- `src/kernel.c`: Main kernel logic.
- `src/shell.c`: Advanced Shell (History, Cursor).
- `src/editor.c`: Vix Text Editor.
- `src/fat12.c`: FAT12 Filesystem driver (Case-preserving).
- `src/elf.c`: ELF Binary Loader.
- `src/syscall.c`: System Calls.
- `src/timer.c`: PIT Driver.
- `src/speaker.c`: PC Speaker Driver.
- `src/keyboard.c`: Scancode to ASCII & Arrow keys.
- `src/gdt.c`: GDT & TSS.
- `src/idt.c`: IDT.
- `src/paging.c`: Memory Paging.
- `src/pmm.c`: Physical Memory Manager.
- `src/kheap.c`: Kernel Heap.

## Disclaimer
This OS is for educational purposes only.
It is not intended for production use.