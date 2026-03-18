CC = gcc
AS = as
LD = ld

# Compiler flags for 32-bit C code, freestanding (no stdlib)
# Added -Isrc/include to find headers
CFLAGS = -m32 -ffreestanding -O2 -Wall -Wextra -std=gnu99 -fno-pie -Isrc/include

# Assembler flags for 32-bit
ASFLAGS = --32

# Linker flags to simulate i386 ELF
LDFLAGS = -m elf_i386 -T linker.ld -nostdlib

# Source files (updated paths)
# Using wildcards to automatically find files in the new structure
SOURCES_C = $(wildcard src/kernel/*.c) \
            $(wildcard src/arch/i386/*.c) \
            $(wildcard src/drivers/*.c) \
            $(wildcard src/mm/*.c) \
            $(wildcard src/fs/*.c) \
            $(wildcard src/bin/*.c)

SOURCES_S = $(wildcard src/arch/i386/*.s)

# Object files
OBJECTS = $(SOURCES_C:.c=.o) $(SOURCES_S:.s=.o)

# Output kernel binary
KERNEL = jexos.bin
IMG = jexos.img

all: $(KERNEL) $(IMG)

$(KERNEL): $(OBJECTS)
	$(LD) $(LDFLAGS) -o $@ $(OBJECTS)

$(IMG): tools/mkjexfs.c
	gcc tools/mkjexfs.c -o tools/mkjexfs
	./tools/mkjexfs $(IMG)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

%.o: %.s
	$(AS) $(ASFLAGS) $< -o $@

run: $(KERNEL) $(IMG)
	qemu-system-i386 -kernel $(KERNEL) -hda $(IMG) -serial stdio -machine pcspk-audiodev=audio0 -audiodev pa,id=audio0

clean:
	rm -f $(OBJECTS) $(KERNEL) $(IMG) tools/mkjexfs

.PHONY: all run clean
