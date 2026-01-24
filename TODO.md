# JexOS 0.2 Roadmap: The Self-Hosting Dream

The goal of version 0.2 is to allow JexOS to compile and run C programs natively.

## 🛠️ Core Objectives

### 1. Minimal LibC (Standard C Library)
To run a compiler like TCC, we need standard functions. We cannot implement *everything*, but we need the basics:
- [x] `stdio.h`: `printf`, `fopen`, `fclose`, `fread`, `fwrite`.
- [x] `stdlib.h`: `malloc`, `free`, `exit`, `atoi`.
- [x] `string.h`: `memcpy`, `memset`, `strcmp`, `strcpy`, `strlen`.
- [x] `unistd.h`: `open`, `close`, `read`, `write`, `sbrk` (for malloc).

### 2. TCC (Tiny C Compiler) Port
We need to compile TCC so it runs on JexOS.
- [x] **ELF Loader Upgrade**: Handled .bss, stack mapping, and correct entry point resolution.
- [x] **Relocation Support**: Basic support added in elf.c.
- [x] **TCC Integration**: TCC is now integrated into the kernel shell for native compilation.
- [ ] **Cross-Compilation**: (Optional now that native TCC works).

### 3. Vix Editor 2.0
The editor needs upgrades to be useful for coding.
- [x] **Line Numbers**: Implemented.
- [x] **Proper Editing**: Support for inserting/deleting at cursor.
- [ ] **Syntax Highlighting**: (Maybe simple keyword coloring).
- [ ] **Scrolling Fixes**: Better handling of large files.

## 🗓️ Execution Plan

1.  **Step 1**: Build `libc.a` (a static library) wrapping our Syscalls. [DONE]
2.  **Step 2**: Compile a simple "Hello World" C program on Linux using that `libc` and run it on JexOS. [DONE]
3.  **Step 3**: Port TCC. [DONE]
4.  **Step 4**: Compile a program *inside* JexOS using `tcc hello.c -o hello`. [DONE]

## 🚀 Target
**"I write a C code on my kernel, I compile it on my kernel, and I run it on my kernel."** [ACHIEVED]