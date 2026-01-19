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

### Prerequisites

You need `gcc`, `nasm` (or `as`), `ld`, and `qemu-system-i386`.

### Building

```bash
make
```

### Running

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
