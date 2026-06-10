CC = gcc
AS = as
LD = ld

# Compiler flags for 32-bit C code, freestanding (no stdlib)
# Added -Isrc/include to find headers
CFLAGS = -m32 -ffreestanding -O2 -Wall -Wextra -std=gnu99 -fno-pie -Isrc/include -Isrc/tests

# Assembler flags for 32-bit
ASFLAGS = --32

# Linker flags to simulate i386 ELF
LDFLAGS = -m elf_i386 -T linker.ld -nostdlib

# GRUB paths (Fedora)
GRUB_MODULES = /usr/lib/grub/i386-pc
GRUB_BIN = /usr/bin/grub2-mkimage

# Source files (updated paths)
# Using wildcards to automatically find files in the new structure
SOURCES_C = $(wildcard src/kernel/*.c) \
            $(wildcard src/arch/i386/*.c) \
            $(wildcard src/drivers/*.c) \
            $(wildcard src/mm/*.c) \
            $(wildcard src/fs/*.c) \
            $(wildcard src/bin/*.c) \
            $(wildcard src/debug/*.c) \
            $(filter-out src/lib/type.c, $(wildcard src/lib/*.c)) \
            $(wildcard src/tests/*.c)

SOURCES_S = $(wildcard src/arch/i386/*.s)

# Object files
OBJECTS = $(SOURCES_C:.c=.o) $(SOURCES_S:.s=.o)

# Output files
KERNEL = jexos.bin
IMG = jexos.img
ISO = jexos.iso

all: $(KERNEL) $(IMG)

$(KERNEL): $(OBJECTS) tools/gen_kallsyms
	$(LD) $(LDFLAGS) -o $@ $(OBJECTS)
	nm -n -S $@ | grep -E ' T ' | sort | ./tools/gen_kallsyms > src/kernel/kallsyms_data.S
	$(AS) $(ASFLAGS) src/kernel/kallsyms_data.S -o src/kernel/kallsyms_data.o
	$(LD) $(LDFLAGS) -o $@ $(OBJECTS) src/kernel/kallsyms_data.o

tools/gen_kallsyms: tools/gen_kallsyms.c
	$(CC) -o $@ $<

$(IMG): tools/mkjexfs.c
	gcc tools/mkjexfs.c -o tools/mkjexfs
	./tools/mkjexfs $(IMG)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

%.o: %.s
	$(AS) $(ASFLAGS) $< -o $@

# Build bootable ISO with GRUB using grub2-mkrescue (proper tool for this)
$(ISO): $(KERNEL) $(IMG)
	@mkdir -p iso/boot/grub
	cp $(KERNEL) iso/boot/
	cp $(IMG) iso/boot/
	@if [ ! -f iso/boot/grub/grub.cfg ]; then \
		echo 'set timeout=5' > iso/boot/grub/grub.cfg; \
		echo 'menuentry "JexOS" {' >> iso/boot/grub/grub.cfg; \
		echo '    multiboot /boot/jexos.bin' >> iso/boot/grub/grub.cfg; \
		echo '    boot' >> iso/boot/grub/grub.cfg; \
		echo '}' >> iso/boot/grub/grub.cfg; \
	fi
	@echo "Building ISO with GRUB..."
	@grub2-mkrescue -o $(ISO) iso || (echo "grub2-mkrescue failed" && exit 1)
	@echo "ISO built: $(ISO)"

# Run with GRUB ISO (full PCI access!)
run-iso: $(ISO)
	qemu-system-i386 -cdrom $(ISO) -m 128 -serial stdio -machine pcspk-audiodev=audio0 -audiodev pa,id=audio0 -netdev user,id=net0 -device rtl8139,netdev=net0

# Original run (kernel mode)
run: $(KERNEL) $(IMG)
	qemu-system-i386 -kernel $(KERNEL) -hda $(IMG) -serial stdio -machine pcspk-audiodev=audio0 -audiodev pa,id=audio0 -netdev user,id=net0 -device rtl8139,netdev=net0

clean:
	rm -f $(OBJECTS) $(KERNEL) $(IMG) $(ISO) tools/mkjexfs tools/gen_kallsyms src/kernel/kallsyms_data.S src/kernel/kallsyms_data.o
	rm -rf iso/

.PHONY: all run run-iso clean jexos.iso
