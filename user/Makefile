CC = gcc
AS = as
AR = ar
CFLAGS = -isystem /usr/lib/gcc/x86_64-linux-gnu/13/include -m32 -ffreestanding -nostdlib -nostdinc -fno-builtin -fno-stack-protector -Iinclude -O2
LDFLAGS = -m elf_i386 -T user.ld

# Library sources
LIB_OBJS = lib/stdio.o lib/string.o lib/stdlib.o lib/unistd.o lib/crt0.o

all: hello.elf test_stdlib.elf test_unistd.elf

# Build LibC
libc.a: $(LIB_OBJS)
	$(AR) rcs $@ $^

# Build Hello App
hello.elf: ../apps/hello.c libc.a
	$(CC) $(CFLAGS) -c $< -o hello.o
	ld $(LDFLAGS) lib/crt0.o hello.o libc.a -o $@

# Build stdlib test app
test_stdlib.elf: ../apps/test_stdlib.c libc.a
	$(CC) $(CFLAGS) -c $< -o test_stdlib.o
	ld $(LDFLAGS) lib/crt0.o test_stdlib.o libc.a -o $@

# Build unistd test app
test_unistd.elf: ../apps/test_unistd.c libc.a
	$(CC) $(CFLAGS) -c $< -o test_unistd.o
	ld $(LDFLAGS) lib/crt0.o test_unistd.o libc.a -o $@

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

%.o: %.s
	$(AS) --32 $< -o $@

clean:
	rm -f $(LIB_OBJS) hello.o hello.elf test_stdlib.o test_stdlib.elf test_unistd.o test_unistd.elf libc.a

.PHONY: all clean
