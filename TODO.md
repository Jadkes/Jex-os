# JexOS 0.2 Roadmap: The Self-Hosting Dream

The goal of version 0.2 is to allow JexOS to compile and run C programs natively.

## 🛠️ Core Objectives

### 1. Minimal LibC (Standard C Library)
To run a compiler like TCC, we need standard functions. We cannot implement *everything*, but we need the basics:
- [x] `stdio.h`: `printf`, `fopen`, `fclose`, `fread`, `fwrite`.
- [ ] `stdlib.h`: `malloc`, `free`, `exit`, `atoi`.
- [x] `string.h`: `memcpy`, `memset`, `strcmp`, `strcpy`, `strlen`.
- [ ] `unistd.h`: `open`, `close`, `read`, `write`, `sbrk` (for malloc).

### 2. TCC (Tiny C Compiler) Port
We need to compile TCC so it runs on JexOS.
- [ ] **ELF Loader Upgrade**: Our current loader is simple. TCC might need `.bss` handling, command-line arguments (`argc`, `argv`), and possibly relocation support.
- [ ] **Cross-Compilation**: We will compile TCC on Linux *targeting* JexOS, then drop the `tcc` binary onto the FAT12 disk.
- [ ] **Headers**: We need to put our `libc` headers (`.h` files) on the JexOS disk so TCC can find them when compiling.

### 3. Vix Editor 2.0
The editor needs upgrades to be useful for coding.
- [ ] **Line Numbers**: Essential for debugging compiler errors.
- [ ] **Syntax Highlighting**: (Maybe simple keyword coloring).
- [ ] **Scrolling Fixes**: Better handling of large files.
- [ ] **Copy/Paste**: Basic buffer support.

## 🗓️ Execution Plan

1.  **Step 1**: Build `libc.a` (a static library) wrapping our Syscalls.
2.  **Step 2**: Compile a simple "Hello World" C program on Linux using that `libc` and run it on JexOS.
3.  **Step 3**: Port TCC.
4.  **Step 4**: Compile a program *inside* JexOS using `tcc hello.c -o hello`.

## 🚀 Target
**"I write a C code on my kernel, I compile it on my kernel, and I run it on my kernel."**
