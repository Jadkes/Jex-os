# JexOS 0.5 Roadmap: Peak UX & Games

The goal of version 0.5 is to make JexOS fun and highly usable. We've built the foundations (multitasking, persistence, isolation); now we build the experience.

## 🛠️ Core Objectives

### 1. Shell & User Experience (UX)
Make the shell feel like a professional Linux terminal.
- [ ] **Tab Completion**: Auto-complete filenames and commands when pressing TAB.
- [ ] **Command Aliases**: Support `alias ll='ls -l'`.
- [ ] **Top**: A real-time process monitor showing CPU and memory usage per PID.
- [ ] **Uptime**: A command to see how long JexOS has been "alive".

### 2. BSD-inspired Terminal Games
Bringing entertainment to the kernel.
- [ ] **JexSnake**: A classic snake game built specifically for the JexOS terminal.
- [ ] **2048**: A terminal port of the popular puzzle game.
- [ ] **Worm**: A vintage BSD-style game.

### 3. Power Utilities
- [ ] **Grep**: Search for strings within files (`cat file.c | grep printf`).
- [ ] **Find**: Locate files by name across the directory tree.
- [ ] **Clear Improved**: Add `Ctrl+L` shortcut to clear the screen.

### 4. System Stability (v0.5 Under-the-Hood)
- [ ] **Keyboard Buffering**: Improve the driver so typing during high-load (like during compilation) doesn't drop keys.
- [ ] **Dynamic Environment**: A basic `env` system for `PATH` support.

## 🗓️ Execution Plan

1.  **Step 1**: Implement Tab Completion in the shell.
2.  **Step 2**: Build the `uptime` and `top` commands.
3.  **Step 3**: Port or create JexSnake.
4.  **Step 4**: Implement `grep` for basic text searching.

## 🚀 Target
**"Play Snake while my C code compiles in the background."**
