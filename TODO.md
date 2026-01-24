# JexOS 0.4 Roadmap: The Multitasking Era

The goal of version 0.4 is to transform JexOS from a single-tasking OS into a true multitasking system with process isolation and Inter-Process Communication (IPC).

## 🛠️ Core Objectives

### 1. Multitasking & Scheduling
Currently, JexOS can only run one process at a time. We need a scheduler to manage multiple tasks.
- [ ] **Task State Segment (TSS)**: Finish implementing TSS for stable hardware context switching.
- [ ] **Process Control Block (PCB)**: Create a `struct task` to store registers, PID, state, and memory maps.
- [ ] **Round-Robin Scheduler**: Implement a Timer-based (IRQ0) scheduler to switch between processes.
- [ ] **`fork()` Syscall**: Implement process cloning (the "Linux 0.11 way").
- [ ] **`waitpid()` & `exit()`**: Proper parent-child process synchronization.

### 2. Virtual Memory Isolation
Moving beyond identity mapping to ensure processes cannot crash each other.
- [ ] **Process Page Directories**: Give every process its own 4GB virtual address space.
- [ ] **Kernel Mirroring**: Map the kernel into the higher half (or lower half) of every process's page table.
- [ ] **Copy-on-Write (COW)**: (Advanced) Optimize `fork()` by sharing pages until they are modified.

### 3. File System Enhancements (JexFS)
Moving from basic operations to a full Unix-like file management experience.
- [ ] **Standard Commands**: Implement `mv` (rename), `cp` (copy), `rm` (delete), and `mkdir` (create directory).
- [ ] **Persistent Block Management**: Improve the block allocator to reuse freed blocks.
- [ ] **Path Support**: Support nested directories (e.g., `/bin/hello`).

### 4. Inter-Process Communication (IPC)
Allowing processes to talk to each other.
- [ ] **Pipes**: Implement `pipe()` syscall for streaming data between processes.
- [ ] **Shell Redirection**: Update the shell to support `ls | cat` style commands.

### 4. Vix Editor 3.0 & LibC
- [ ] **Syntax Highlighting**: Add support for more colors (comments, numbers).
- [ ] **Scrolling**: Fix the editor to handle files larger than 25 lines.
- [ ] **`signal.h`**: Implement basic signal handling (SIGINT, SIGKILL).

## 🗓️ Execution Plan

1.  **Step 1**: Implement the PCB structure and a manual context switch function.
2.  **Step 2**: Hook the scheduler into the Timer Interrupt (IRQ0).
3.  **Step 3**: Implement `fork()` and test running two "Hello World" loops at once.
4.  **Step 4**: Implement Virtual Memory isolation so each process has its own stack and heap.

## 🚀 Target
**"Run a compiler in the background while I continue writing code in the foreground."**
