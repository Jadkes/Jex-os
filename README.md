# JexOS

A minimal 32-bit Operating System built for learning purposes.

## Features

- **Multiboot Compliant**: Boots with GRUB or QEMU.
- **32-bit Protected Mode**: Uses GDT and IDT.
- **User Mode (Ring 3)**: Implemented TSS and privilege separation.
- **System Calls**: `int 0x80` interface for user-to-kernel communication.
- **Interrupt Handling**: Custom ISRs and IRQs.
- **Keyboard Driver**: Fully functional with backspace and **Shift** key support.
- **VGA Text Mode**: Scrolling, newline support, and hardware cursor.
- **Filesystem**: FAT12 RAM-disk with basic file operations.
- **Shell**: `root@jexos:/>` prompt with commands:
  - `help`: List commands.
  - `ls`, `touch`, `cat`, `echo`, `rm`: File management.
  - `clear`: Clear the screen.
  - `logo`: Display ASCII art.
  - `time`/`date`: Real-time clock support.
  - `free`: Physical memory status.
  - `usermode`: Test Ring 3 transition.
  - `reboot`/`shutdown`: Power management.
  - `pagefault`/`panic`: Stability testing.
- **Memory Management**:
  - **PMM**: Physical Memory Manager (Bitmap-based).
  - **Paging**: Identity mapping (first 128MB) to prevent faults.
  - **KHeap**: Kernel heap allocator for dynamic data.
- **Kernel Panic**: Custom "Red Screen of Death" for unhandled exceptions.

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

#### **Termux / Android (PKG)**
```bash
pkg install build-essential qemu-system-x86-headless git
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

## Project Structure

- `src/boot.s`: Assembly entry point.
- `src/kernel.c`: Main kernel logic.
- `src/gdt.c/h`: Global Descriptor Table & TSS.
- `src/idt.c/h`: Interrupt Descriptor Table.
- `src/isr.c/h`: Interrupt Service Routines.
- `src/irq.c/h`: Hardware Interrupts (PIC).
- `src/keyboard.c/h`: Keyboard driver.
- `src/shell.c/h`: JexShell implementation.
- `src/pmm.c/h`: Physical Memory Manager.
- `src/paging.c/h`: Paging implementation.
- `src/kheap.c/h`: Kernel Heap allocator.
- `src/fat12.c/h`: FAT12 Filesystem driver.
- `src/syscall.c/h`: System call interface.
- `src/usermode.s`: Ring 3 transition logic.
- `src/power.c/h`: Reboot and Shutdown handlers.
- `src/rtc.c/h`: Real Time Clock driver.
- `src/ports.h`: I/O port helpers.
