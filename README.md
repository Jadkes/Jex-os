# JexOS

A minimal 32-bit Operating System built for learning purposes.

## 🚀 Latest Update: The Sound of Victory!
JexOS now features a fully functional **PC Speaker Driver**, allowing the kernel to generate tones and play music. This update also includes a refined boot logo and improved system stability.

## Features

- **Audio Support**: PC Speaker driver with `beep` and `music` commands (plays the iconic victory fanfare!).
- **Multiboot Compliant**: Boots with GRUB or QEMU.
- **32-bit Protected Mode**: Uses GDT, IDT, and TSS.
- **User Mode (Ring 3)**: Full privilege separation with stable system calls (`int 0x80`).
- **Interrupt Handling**: Custom ISRs and IRQs (Keyboard, Timer, RTC).
- **Keyboard Driver**: Fully functional with backspace, **Shift** key support, and special characters.
- **VGA Text Mode**: Scrolling, newline support, hardware cursor, and Cyan-themed ASCII art.
- **Filesystem**: FAT12 RAM-disk with basic file operations (`ls`, `touch`, `cat`, `echo`, `rm`).
- **Memory Management**:
  - **PMM**: Physical Memory Manager (Bitmap-based).
  - **Paging**: Identity mapping (first 128MB) for absolute stability.
  - **KHeap**: Kernel heap allocator for dynamic data.
- **Power Management**: Functional `reboot` and `shutdown` commands.

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
- `src/speaker.c/h`: PC Speaker audio driver.
- `src/timer.c/h`: PIT timer and sleep implementation.
- `src/fat12.c/h`: FAT12 Filesystem driver.
- `src/syscall.c/h`: System call interface.
- `src/usermode.s`: Ring 3 transition logic.
- `src/shell.c/h`: JexShell with argument parsing.
- `src/kernel.c`: Main kernel logic.
- `src/gdt.c/h`: Global Descriptor Table & TSS.
- `src/idt.c/h`: Interrupt Descriptor Table.
- `src/isr.c/h`: Interrupt Service Routines.
- `src/irq.c/h`: Hardware Interrupts (PIC).
- `src/keyboard.c/h`: Keyboard driver.
- `src/pmm.c/h`: Physical Memory Manager.
- `src/paging.c/h`: Paging implementation.
- `src/kheap.c/h`: Kernel Heap allocator.
- `src/rtc.c/h`: Real Time Clock driver.
- `src/ports.h`: I/O port helpers.
- `src/power.c/h`: Reboot and Shutdown handlers.

## Disclaimer
This OS is for educational purposes only.
It is not intended for production use.
