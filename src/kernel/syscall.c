/**
 * @file syscall.c
 * @brief System Call Dispatcher.
 *
 * Implements the handler for 'int 0x80' which allows user-space programs
 * to request services from the kernel.
 */

#include "syscall.h"
#include "task.h"
#include "idt.h"
#include "gdt.h"
#include "init.h"
#include "fs.h"
#include <stdint.h>

/**
 * @brief Validate that a user-provided pointer lies in user-space memory.
 *
 * User-space is < 0xC0000000. Kernel-space starts at 0xC0000000.
 * A NULL pointer (0) is also rejected as invalid for most operations.
 *
 * @param ptr User-provided pointer
 * @param len Expected accessible length (for bounds check)
 * @return 1 if valid, 0 if invalid
 */
static int is_valid_user_ptr(const void* ptr, uint32_t len) {
    uint32_t addr = (uint32_t)ptr;
    /* Reject NULL, kernel-space, and wrap-around */
    if (addr == 0 || addr >= 0xC0000000) return 0;
    if (addr + len < addr) return 0; /* Overflow/wrap-around */
    if (addr + len > 0xC0000000) return 0; /* Crosses into kernel space */
    return 1;
}

/* Upper bounds keep the page-probe loops below finite no matter what a
 * hostile caller passes in. */
#define MAX_USER_IO_BYTES   (16 * 1024 * 1024)
#define MAX_USER_STR_LEN    4096
#define MAX_USER_ARGV_COUNT 64

extern int paging_is_mapped(const void* virtualaddr);

/**
 * @brief Check a user buffer for range AND mapping.
 *
 * is_valid_user_ptr alone only proves the address lies below 0xC0000000 —
 * an unmapped hole would still fault inside fs_read with kernel privileges.
 * This walks every page the buffer touches and demands it be present in
 * the calling task's directory.
 */
static int user_io_ok(const void* buf, uint32_t len) {
    if (!is_valid_user_ptr(buf, len)) return 0;
    if (len > MAX_USER_IO_BYTES) return 0;
    if (len == 0) return 1;

    uint32_t addr = (uint32_t)buf;
    uint32_t last = addr + len - 1;
    for (uint32_t page = addr & ~0xFFFu; ; page += 0x1000) {
        if (!paging_is_mapped((const void*)page)) return 0;
        if (page >= last) break;
    }
    return 1;
}

/**
 * @brief Check that a NUL-terminated user string is fully readable.
 *
 * Probes each new page before reading bytes from it, so a string whose
 * tail sits in unmapped memory fails validation instead of faulting.
 */
static int user_str_ok(const char* str) {
    if (!is_valid_user_ptr(str, 1)) return 0;
    uint32_t addr = (uint32_t)str;
    if (!paging_is_mapped(str)) return 0;

    for (uint32_t i = 0; i < MAX_USER_STR_LEN; i++) {
        if (((addr + i) & 0xFFF) == 0 && !paging_is_mapped(str + i)) return 0;
        if (str[i] == '\0') return 1;
    }
    return 0; /* ran off without a terminator */
}

/**
 * @brief Check a NULL-terminated argv/envp array of user strings.
 */
static int user_argv_ok(char** argv) {
    if (!argv) return 1; /* NULL argv/envp is legal */

    for (int i = 0; i < MAX_USER_ARGV_COUNT; i++) {
        if (!paging_is_mapped(&argv[i])) return 0;
        char* arg = argv[i];
        if (!arg) return 1; /* terminator reached */
        if (!user_str_ok(arg)) return 0;
    }
    return 0; /* no terminator within the cap */
}

extern void terminal_writestring(const char* data);
extern void isr128();

/**
 * @brief Main System Call Handler.
 * Dispatched by isr_handler in isr.c when int_no is 128 (0x80).
 * 
 * @param regs State of CPU registers containing syscall number and arguments.
 */
void syscall_handler(registers_t *regs)
{
    /* The syscall number is passed in EAX */
    if (regs->eax == SYS_PRINT)
    {
        /* EBX = Pointer to string */
        if (!user_str_ok((const char*)regs->ebx))
            return;
        terminal_writestring((const char*)regs->ebx);
    }
    else if (regs->eax == SYS_EXIT)
    {
        /* EBX = exit code. _task_exit marks this task ZOMBIE and switches. */
        _task_exit((int)regs->ebx);
    }
    else if (regs->eax == SYS_OPEN)
    {
        /* EBX = filename, ECX = flags */
        if (!user_str_ok((const char*)regs->ebx)) {
            regs->eax = -1;
            return;
        }
        regs->eax = fs_open((const char*)regs->ebx, regs->ecx);
    }
    else if (regs->eax == SYS_READ)
    {
        /* EBX = fd, ECX = buffer, EDX = size */
        if (!user_io_ok((void*)regs->ecx, regs->edx)) {
            regs->eax = -1;
            return;
        }
        regs->eax = fs_read(regs->ebx, (void*)regs->ecx, regs->edx);
    }
    else if (regs->eax == SYS_WRITE)
    {
        /* EBX = fd, ECX = buffer, EDX = size */
        if (!user_io_ok((const void*)regs->ecx, regs->edx)) {
            regs->eax = -1;
            return;
        }
        regs->eax = fs_write(regs->ebx, (const void*)regs->ecx, regs->edx);
    }
    else if (regs->eax == SYS_CLOSE)
    {
        /* EBX = fd */
        fs_close(regs->ebx);
        regs->eax = 0;
    }
    else if (regs->eax == SYS_SEEK)
    {
        /* EBX = fd, ECX = offset, EDX = whence */
        regs->eax = fs_seek(regs->ebx, regs->ecx, regs->edx);
    }
    else if (regs->eax == SYS_SBRK)
    {
        /* EBX = increment */
        extern void* sbrk(intptr_t increment);
        regs->eax = (uint32_t)sbrk(regs->ebx);
    }
    else if (regs->eax == SYS_EXECVE)
    {
        /* EBX = filename, ECX = argv, EDX = envp */
        if (!user_str_ok((const char*)regs->ebx) ||
            !user_argv_ok((char**)regs->ecx) ||
            !user_argv_ok((char**)regs->edx)) {
            regs->eax = -1;
            return;
        }
        const char* filename = (const char*)regs->ebx;
        char** argv = (char**)regs->ecx;
        char** envp = (char**)regs->edx;

        extern int execve_file(const char* filename, char** argv, char** envp);
        regs->eax = execve_file(filename, argv, envp);
    }
    else if (regs->eax == SYS_FORK)
    {
        regs->eax = fork(regs);
    }
    else if (regs->eax == SYS_WAITPID)
    {
        /* EBX = pid, ECX = status ptr, EDX = options */
        if (!is_valid_user_ptr((void*)regs->ecx, sizeof(int))) {
            regs->eax = -1;
            return;
        }
        regs->eax = waitpid((int)regs->ebx, (int*)regs->ecx, (int)regs->edx);
    }
    else if (regs->eax == SYS_GETPID)
    {
        regs->eax = getpid();
    }
    else if (regs->eax == SYS_PRINT_INT)
    {
        int val = (int)regs->ebx;
        char buf[32];
        int i = 0;
        int is_neg = 0;
        /* Handle INT_MIN safely: negating INT_MIN is UB */
        if ((unsigned int)val == 0x80000000) {
            terminal_writestring("-2147483648");
            return;
        }
        if (val == 0) {
            terminal_writestring("0");
        } else {
            if (val < 0) {
                is_neg = 1;
                val = -val;
            }
            while (val > 0) {
                buf[i++] = (val % 10) + '0';
                val /= 10;
            }
            if (is_neg) buf[i++] = '-';
            char rev[32];
            for (int j = 0; j < i; j++) {
                rev[j] = buf[i - 1 - j];
            }
            rev[i] = '\0';
            terminal_writestring(rev);
        }
    }
    else if (regs->eax == SYS_PRINT_CHAR)
    {
        char c = (char)regs->ebx;
        char str[2] = {c, '\0'};
        terminal_writestring(str);
    }
    else if (regs->eax == SYS_PRINT_HEX)
    {
        uint32_t val = (uint32_t)regs->ebx;
        char buf[11]; /* "0x" + 8 hex digits + null */
        const char* hex = "0123456789ABCDEF";
        int i = 0;
        buf[i++] = '0';
        buf[i++] = 'x';
        int started = 0;
        for (int shift = 28; shift > 0; shift -= 4) {
            int digit = (val >> shift) & 0xF;
            if (digit || started) {
                buf[i++] = hex[digit];
                started = 1;
            }
        }
        buf[i++] = hex[val & 0xF];
        buf[i] = '\0';
        terminal_writestring(buf);
    }
    else if (regs->eax == SYS_SIGNAL)
    {
        /* EBX = sig, ECX = handler (SIG_DFL/SIG_IGN sentinels allowed) */
        if ((uint32_t)regs->ecx >= 0xC0000000) {
            regs->eax = (uint32_t)-1;
            return;
        }
        regs->eax = (uint32_t)sys_signal((int)regs->ebx, (void*)regs->ecx);
    }
    else if (regs->eax == SYS_KILL)
    {
        /* EBX = pid, ECX = sig */
        regs->eax = (uint32_t)sys_kill((int)regs->ebx, (int)regs->ecx);
    }
}

/**
 * @brief Register the syscall handler in the IDT.
 */
void init_syscalls()
{
    /* Gate 0x80: DPL=3 so user code can int $0x80 */
    idt_set_gate(SYSCALL_VECTOR, (uint32_t)isr128, SEG_KERNEL_CODE, SYSCALL_GATE_ACCESS);
}

device_init(init_syscalls);
