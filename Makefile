CC = gcc
AS = as
LD = ld

# Compiler flags for 32-bit C code, freestanding (no stdlib)
CFLAGS = -m32 -ffreestanding -O2 -Wall -Wextra -std=gnu99 -fno-pie

# Assembler flags for 32-bit
ASFLAGS = --32

# Linker flags to simulate i386 ELF
LDFLAGS = -m elf_i386 -T linker.ld -nostdlib

# Source files
SOURCES_C = src/kernel.c src/gdt.c src/isr.c src/idt.c src/irq.c src/keyboard.c src/shell.c src/rtc.c src/pmm.c src/paging.c src/kheap.c src/fat12.c src/power.c
SOURCES_S = src/boot.s src/gdt_flush.s src/interrupts.s

# Object files
OBJECTS = $(SOURCES_C:.c=.o) $(SOURCES_S:.s=.o)

# Output kernel binary
KERNEL = jexos.bin

all: $(KERNEL)

$(KERNEL): $(OBJECTS)
	$(LD) $(LDFLAGS) -o $@ $(OBJECTS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

%.o: %.s
	$(AS) $(ASFLAGS) $< -o $@

run: $(KERNEL)
	qemu-system-i386 -kernel $(KERNEL)

clean:
	rm -f $(OBJECTS) $(KERNEL)

.PHONY: all run clean
