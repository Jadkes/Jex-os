# JexOS

A minimal 32-bit Operating System built for learning purposes.

## Features

- **Multiboot Compliant**: Boots with GRUB or QEMU.
- **32-bit Protected Mode**: Uses GDT and IDT.
- **Interrupt Handling**: Custom ISRs and IRQs.
- **Keyboard Driver**: Fully functional with backspace support.
- **VGA Text Mode**: Scrolling and newline support.
- **Shell**: `JexShell` with commands:
  - `help`: List commands.
  - `clear`: Clear the screen.
  - `logo`: Display ASCII art.
  - `time`/`date`: Real-time clock support.
  - `free`: Physical memory status.
  - `pagefault`: Paging test.
- **Memory Management**:
  - **PMM**: Physical Memory Manager (Bitmap-based).
  - **Paging**: Identity mapping (first 4MB) to prevent triple faults.
- **Kernel Panic**: Custom "Red Screen of Death" for unhandled exceptions.

## How to Build and Run

### 1. Installation (For Debian/Ubuntu/Kali)

Open your terminal and run the following command to install all necessary tools:

```bash
sudo apt update && sudo apt install -y build-essential qemu-system-x86 git
```

### 2. Download the Project

Clone the repository to your local machine:

```bash
git clone https://github.com/inasj-cyber/Jex-os.git
cd Jex-os
```

### 3. Building the OS

Compile the kernel and assembly files:

```bash
make
```

### 4. Running the OS

Launch the OS in the QEMU emulator:

```bash
make run
```

## Project Structure

- `src/boot.s`: Assembly entry point.
- `src/kernel.c`: Main kernel logic.
- `src/gdt.c/h`: Global Descriptor Table.
- `src/idt.c/h`: Interrupt Descriptor Table.
- `src/isr.c/h`: Interrupt Service Routines.
- `src/irq.c/h`: Hardware Interrupts.
- `src/keyboard.c/h`: Keyboard driver.
- `src/shell.c/h`: JexShell implementation.
- `src/pmm.c/h`: Physical Memory Manager.
- `src/paging.c/h`: Paging implementation.
- `src/rtc.c/h`: Real Time Clock driver.
- `src/ports.h`: I/O port helpers.