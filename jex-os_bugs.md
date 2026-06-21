# JexOS Bug Report

After auditing the full jex-os repository (https://github.com/Jadkes/jex-os),
here are the bugs I found, organized by severity.

================================================================
## CRITICAL — Cause wrong behavior or crashes
================================================================

### 1. Array indexing is completely broken (expr.c:424 + symtab.c:80-93)

`symtab_add_array()` always stores `SYM_ARRAY` as the type and discards the
element-type parameter. Then in `expr.c`, `sym_type_size(sym->type)` is called
on the array symbol — but `sym_type_size(SYM_ARRAY)` returns 0.

The check `if (elem_size == 4)` in expr.c:425 is therefore ALWAYS false, so
the index is never scaled by 4 for int arrays. `arr[5]` computes
`base + 5` instead of `base + 20`. Every int-array access reads the wrong
memory location.

### 2. register_external_symbol / add_relocation share a counter (tcc.c:712-744)

`register_external_symbol` uses `reloc_count` as the index into `func_table`
(which is sized `MAX_FUNCS=32`), and increments `reloc_count` after each call.
`add_relocation` ALSO uses `reloc_count` as the index into `reloc_table`
(sized `MAX_RELOCS=64`).

Result: the two arrays share a single counter. The first external symbol
goes to `func_table[0]`; the first relocation goes to `reloc_table[1]` (not
`[0]`). After 32 external symbols, `func_table` overflows into whatever
memory follows it.

### 3. ELF relocations all resolve to the entry point (elf.c:86)

```c
uint32_t sym_addr = header->e_entry;  /* "For now, assume simplified" */
apply_relocation(reloc_addr, sym_addr, rel_type);
```

Every single relocation in a relocatable ELF is patched to point at the
entry point. Any `call printf` becomes `call _start`. Code jumps to wrong
addresses and crashes immediately.

### 4. Syscall handler does no pointer validation (syscall.c:25-136)

Every syscall that takes a user pointer trusts it blindly:
- SYS_PRINT (EBX), SYS_OPEN (EBX), SYS_READ (ECX), SYS_WRITE (ECX),
  SYS_EXECVE (EBX/ECX/EDX), SYS_WAITPID (ECX)

A malicious or buggy user program can pass a kernel-space pointer (e.g.
0xC0000000) and the kernel will happily read/write kernel memory, or pass
an unmapped pointer and panic the kernel on a page fault.

### 5. `cd` command overflows shell_cwd (shell.c:970-971)

`shell_cwd` is `char[128]`. The `cd` command does:
```c
if (path[0] == '/') strcpy(shell_cwd, path);
else { strcat(shell_cwd, "/"); strcat(shell_cwd, path); }
```
`path` comes from `shell_buffer` which is 256 bytes. A 200-char path
overflows `shell_cwd` by 72+ bytes, corrupting adjacent stack/static data.

### 6. PMM has no bounds check on free (pmm.c:230-255)

`pmm_free_block` and `pmm_free_blocks` compute `frame = addr / BLOCK_SIZE`
and call `mmap_test`/`mmap_unset` without checking `frame < TOTAL_BLOCKS`.
Freeing an address above 128MB (e.g. 0xC0000000 = frame 0xC000) reads/writes
past the 4KB bitmap array.

### 7. RTL8139 RX doesn't handle ring buffer wrap (rtl8139.c:122)

```c
uint8_t* frame = rx_buffer + offset + 4;
net_process_packet(frame, rx_len);
```
If `offset + 4 + rx_len > RX_BUF_SIZE`, the frame wraps around the 32KB
ring buffer, but this code passes a single linear pointer. `net_process_packet`
reads past the buffer end. Real RTL8139 drivers must either copy the wrapped
frame into a linear buffer or use scatter-gather.

================================================================
## HIGH — Security or major correctness issues
================================================================

### 8. Stack offsets > 128 bytes silently truncate (expr.c:109,120,130,165,176,225 + tcc.c:414,441,469,478,497)

Every `mov eax, [ebp+disp8]` instruction uses a signed 8-bit displacement.
The code casts the offset with `(uint8_t)(offset)` or `(uint8_t)(-sym->offset)`.
If a local variable's offset exceeds 127 (i.e. >127 bytes of locals), the
displacement wraps. A variable at `[ebp-200]` becomes `[ebp-200+256]` =
`[ebp+56]`, reading the wrong stack location. Functions with >31 int locals
silently corrupt data.

### 9. TSS limit is set to end address, not size (gdt.c:54)

```c
uint32_t limit = base + sizeof(tss_entry);
gdt_set_gate(num, base, limit, 0xE9, 0x00);
```
The GDT limit field is the last valid byte OFFSET from base, not the end
address. Setting `limit = base + size` makes the CPU think the TSS spans
`base` to `base + base + size` = `2*base + size` bytes. The CPU may read
garbage from memory past the TSS during task switches.

### 10. clone_page_directory leaks on OOM (paging.c:154,164)

When `pmm_alloc_block` fails mid-clone, the function returns NULL without
freeing the page directory frame or any previously-cloned page tables.
In an OOM situation this wastes precious physical pages.

### 11. ptr_to_slab reads arbitrary memory as magic (kheap.c:96-101)

`ptr_to_slab` aligns the pointer to a page boundary and reads `slab->magic`.
If the pointer was NOT allocated by kmalloc (e.g. a stack pointer, or a
pointer from `pmm_alloc_block` directly), this reads whatever 4 bytes
happen to be at the page start. If they coincidentally match SLAB_MAGIC
or LARGE_MAGIC, kfree corrupts memory.

### 12. slab_free has no bounds check on idx (kheap.c:84-94)

`offset / slab->obj_size` could be >= `slab->obj_count` if `ptr` is not
from this slab or alignment is off. The subsequent write to
`data + idx * slab->obj_size` goes outside the slab.

### 13. Scheduler safety counter is wrong (task.c:151)

```c
if (++safety > 100) { /* All tasks are zombies — nothing to run */ }
```
If there are >100 zombie tasks in the ready queue before a runnable task,
the scheduler halts the system — even though a runnable task exists.
The counter should be `total_task_count`, not a hardcoded 100.

### 14. IDE driver has no timeout (ide.c:18-27)

`ide_wait_busy` and `ide_wait_drq` are infinite loops. If the disk fails
(cable unplug, hardware error, power loss), the kernel hangs forever
with no recovery.

### 15. lockdep uses stack indices instead of lock pointers (lockdep.c:120-139,170-175)

The `edges[][]` adjacency matrix is indexed by stack POSITION, not by lock
identity. When locks are released and re-acquired in different order, the
same stack position refers to different locks. Stale edges cause false
deadlock warnings or miss real deadlocks. The matrix is never cleaned
when locks are released.

### 16. ELF loader has no bounds checks (elf.c:65-92,148,152)

- `handle_relocations`: `sh_name` not bounds-checked → OOB read into section
  name table
- `sh_entsize == 0` → division by zero in `rel_count`
- `memcpy((void*)p_vaddr, elf_data + p_offset, p_filesz)` — no check that
  `p_offset + p_filesz` is within the ELF file
- `p_filesz > p_memsz` (malformed ELF) → memcpy writes past mapped region

### 17. vsnprintf INT_MIN handling (vsnprintf.c:24)

`val = (uint32_t)-(int32_t)val;` when `val == 0x80000000` (INT_MIN) is
undefined behavior (signed overflow). The negation stays 0x80000000, so
the subsequent digit loop treats it as a huge unsigned number and prints
garbage.

### 18. syscall SYS_PRINT_INT INT_MIN (syscall.c:105)

Same UB: `val = -val` when `val == INT_MIN`. Output is just "-" with no
digits, or worse.

### 19. fs_read return value not checked in exec (exec.c:192,220)

`fs_read(fd, source, file_size)` may return fewer bytes than requested
(truncated file, I/O error). The code doesn't check, leaving uninitialized
bytes in the buffer. TCC then compiles garbage source code.

================================================================
## MEDIUM — Logic bugs, minor issues
================================================================

### 20. symtab_add_array discards the element type (symtab.c:80-93)

```c
int symtab_add_array(..., sym_type_t type, ...) {
    int result = symtab_add(tab, name, SYM_ARRAY, ...);  /* type ignored */
```
The `type` parameter (the element type) is silently discarded. `int arr[10]`
and `char arr[10]` are indistinguishable. This is the root cause of bug #1.

### 21. scan_dangerous_apis-style issues: comment detection naive (expr.c syntax)

In the editor's `get_char_color`, block comments `/* ... */` are not
detected — only `//` line comments. Code inside block comments is
syntax-highlighted as code.

### 22. strncpy doesn't null-pad (kheap.c:321-326)

The kernel's `strncpy` implementation:
```c
while (n-- && (*d++ = *src++));
if (n) *d = '\0';
```
After the loop, `n` is 0 if the source was longer than `n`. The `if (n)`
check fails, so no null terminator is written. Standard `strncpy` null-
pads the entire remainder. This differs from spec and can cause
unterminated strings if callers expect standard behavior.

### 23. test_fail(NULL) crashes (test_suite.c:49-58)

`test_fail` checks `if (msg)` before strcpy, but then calls
`terminal_writestring(msg)` unconditionally. Passing NULL crashes.

### 24. test_fail strcpy overflow (test_suite.c:53)

`strcpy(fail_msg, msg)` where `fail_msg` is `char[128]`. If `msg` is
longer than 127 chars, buffer overflow.

### 25. page_fault_handler receives struct by value (isr.c:95)

`page_fault_handler(*regs)` copies the entire `registers_t` struct. Any
modifications the handler makes to `regs` (e.g., adjusting EIP to skip
the faulting instruction) are lost. The comment in isr.c:78-81 explicitly
says handlers should be able to modify the register save area, but the
page fault path violates this.

### 26. keyboard_callback potential OOB (keyboard.c:121)

`kbdus[scancode]` and `kbdus_shifted[scancode]` — both arrays are 128
entries, but `scancode` is `uint8_t` (0-255). Break codes are filtered
by `scancode & 0x80`, but a make code with scancode >= 128 (some
extended-key sequences after 0xE0) would access past the array.

### 27. jexfs_read_inode no bounds check (jexfs.c:41-47)

`idx` is not validated. A huge `idx` computes `block` past the inode
table, `read_block` reads garbage, and the caller gets a corrupted inode.

### 28. jexfs_open returns cwd_inode for empty name (jexfs.c:81)

`if (name[0] == '\0') return cwd_inode;` — correct for `cd ""` but wrong
for `open("")`. Should return -1 for non-directory operations.

### 29. map_user_stack leaks on OOM (exec.c:49-51)

If `pmm_alloc_block` fails partway through, all previously-allocated
stack pages are leaked.

### 30. setup_user_stack no bounds check (elf.c:179-206)

`strcpy((char*)esp, argv[i])` — if `argv[i]` is longer than the available
stack space, overflow.

### 31. TCP rx_len race (tcp.c:308-311)

`if (rx_len + copy > TCP_RX_BUF_SIZE) copy = TCP_RX_BUF_SIZE - rx_len;`
If `rx_len` is somehow corrupted to exceed `TCP_RX_BUF_SIZE` (despite
cli/sti), the unsigned subtraction underflows to a huge number and memcpy
overflows.

### 32. fork() comment says child never returns, but code returns 0 (exec.c:146-148)

After `jump_to_user_mode`, the code does `tcc_delete(tcc); return 0;`.
The comment says "Not reached", but if `jump_to_user_mode` ever DOES
return (bug in user-mode transition), `tcc_delete(tcc)` frees memory
that the now-running user program might still reference.

### 33. editor get_char_color O(n²) string scan (editor.c:79-84)

For every character position, it scans from position 0 to `pos` checking
for string quotes. This is O(n²) for the whole file. With an 8KB file,
that's 64M iterations per redraw.

### 34. panic decode_page_fault_err modifies const char* array (panic.c:61)

`while (*parts[i] && pos < buf_len - 2) buf[pos++] = *parts[i]++;`
This advances the pointer (not the string literal), which is legal, but
the pattern is confusing and error-prone.

### 35. keyboard_flush is not atomic (keyboard.c:176-180)

Sets `kbd_head = 0; kbd_tail = 0;` without disabling interrupts. If the
ISR fires between the two assignments, a character could be written to
`kbd_buffer[0]` and then lost when `kbd_tail = 0` resets.

================================================================
## LOW — Style, minor concerns
================================================================

### 36. has_main_function matches comments/strings (symtab.c not, but tcc.c uses string_contains)

Actually this is in c_tester not jex-os. Disregard.

### 37. tokenize_c_code number overflow (tcc.c:167-171)

`val = val * 10 + (source[pos] - '0')` — `val` is `int`. A 10+ digit
number literal overflows. No check.

### 38. tokenize_c_code string literal fixed 256 buffer (tcc.c:177)

`char str_val[256]` — a string literal longer than 255 chars is silently
truncated. Then kmalloc'd and stored.

### 39. tokenize_c_code identifier fixed 64 buffer (tcc.c:201)

`char ident[64]` — identifiers longer than 63 chars are truncated.

### 40. parse_c_tokens fixed 1024 string table (tcc.c:251)

`uint8_t string_table[1024]` — if total string literals exceed 1024 bytes,
`string_pos += len` overflows the buffer silently.

### 41. expr.c arg_positions[32] overflow (expr.c:343)

`int arg_positions[32]` — a function call with >32 arguments overflows.
No bounds check on `arg_count_temp`.

### 42. LOGAND/LOGOR jump offsets are hardcoded (expr.c:819,825,833,839)

The short-circuit code uses fixed byte offsets (0x05, 0x03) for jumps.
If the instruction lengths change (e.g., different register encoding),
the jumps go to wrong addresses. Fragile but currently correct.

### 43. expr.c last_ident_type is global state (expr.c:34)

`last_ident_type`, `last_ident_ptr_size`, `left_is_pointer` are file-static
globals. This makes the parser non-reentrant and breaks if expr_parse is
ever called recursively in a way that clobbers the state before it's read.

### 44. expr.c post-increment of char uses wrong load (expr.c:468)

Post-increment does `emit_mov_eax_mem_ebp` (32-bit load) even for char
variables, then stores back with `emit_mov_mem_ebp_eax` (32-bit store).
This overwrites 3 bytes past the char variable on the stack, corrupting
adjacent locals.

### 45. expr.c prefix ++/-- of char also uses 32-bit ops (expr.c:546,572)

Same issue as #44 — 32-bit load/store on a char variable clobbers
adjacent stack memory.

================================================================

That's 45 bugs across the jex-os codebase. The most severe are:
- #1 (array indexing completely broken)
- #2 (reloc/func table counter sharing)
- #3 (ELF relocations all point to entry)
- #4 (no syscall pointer validation)
- #5 (cd buffer overflow)
- #7 (RTL8139 ring buffer wrap)
- #8 (stack offset truncation)

The OS probably "works" in practice because:
- Arrays are rarely used in the test apps (hello.c uses none)
- External function calls are minimal (print, fork, getpid)
- ELFs loaded are ET_EXEC (no relocations needed)
- User programs are trusted (no malicious input)
- Functions are small (<128 bytes of locals)
- The RTL8139 ring buffer rarely wraps in practice with small packets
