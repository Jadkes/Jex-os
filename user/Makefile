CC = gcc
AS = as
AR = ar
CFLAGS = -isystem /usr/lib/gcc/x86_64-linux-gnu/13/include -m32 -ffreestanding -nostdlib -nostdinc -fno-builtin -fno-stack-protector -Iinclude -O2
LDFLAGS = -m elf_i386 -T user.ld

# Library sources
LIB_OBJS = lib/stdio.o lib/string.o lib/crt0.o

all: hello.elf

# Build LibC
libc.a: $(LIB_OBJS)
	$(AR) rcs $@ $^

# Build Hello App
hello.elf: ../apps/hello.c libc.a
	$(CC) $(CFLAGS) -c $< -o hello.o
	ld $(LDFLAGS) lib/crt0.o hello.o libc.a -o $@

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

%.o: %.s
	$(AS) --32 $< -o $@

clean:
	rm -f $(LIB_OBJS) hello.o hello.elf libc.a

.PHONY: all clean
