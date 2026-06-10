/**
 * @file panic.c
 * @brief Kernel Panic Handler — Crash Screen, Stack Trace, Page Fault Decode.
 *
 * Purpose: Replace basic exception halt with comprehensive diagnostic output
 *          including register dump, page fault decode, stack unwind, and
 *          interactive crash recovery (reboot / save dump / halt).
 * Thread-safety: Interrupts are disabled before any output; no concurrency.
 */

#include "panic.h"
#include "kernel/backtrace.h"
#include "terminal.h"
#include "serial.h"
#include "power.h"
#include "ports.h"
#include "fs.h"
#include <string.h>

/* PS/2 scancode set 1 */
#define KEY_R      0x13
#define KEY_D      0x20
#define KEY_ENTER  0x1C

static const char* exception_names[] = {
    "Division By Zero", "Debug", "NMI", "Breakpoint",
    "Overflow", "Bounds", "Invalid Opcode", "No Coprocessor",
    "Double Fault", "Coprocessor Overrun", "Bad TSS",
    "Segment Not Present", "Stack Fault", "General Protection Fault",
    "Page Fault", "Reserved", "Coprocessor Fault",
    "Alignment Check", "Machine Check", "Reserved",
    "Reserved", "Reserved", "Reserved", "Reserved", "Reserved",
    "Reserved", "Reserved", "Reserved", "Reserved", "Reserved",
    "Reserved", "Reserved"
};

static void print_hex_val(uint32_t val)
{
    char buf[11];
    format_hex(val, buf);
    terminal_writestring(buf);
}

void decode_page_fault_err(uint32_t err, char* buf, int buf_len)
{
    buf[0] = '\0';
    const char* parts[5];
    int count = 0;

    if (!(err & PF_ERR_PRESENT)) parts[count++] = "non-present";
    else                         parts[count++] = "protection";
    if (err & PF_ERR_WRITE)      parts[count++] = "write";
    else                         parts[count++] = "read";
    if (err & PF_ERR_USER)       parts[count++] = "user-mode";
    else                         parts[count++] = "kernel";
    if (err & PF_ERR_RSVD)       parts[count++] = "reserved-bit";
    if (err & PF_ERR_INSTR)      parts[count++] = "instr-fetch";

    int pos = 0;
    for (int i = 0; i < count && pos < buf_len - 2; i++) {
        while (*parts[i] && pos < buf_len - 2)
            buf[pos++] = *parts[i]++;
        if (i < count - 1 && pos < buf_len - 2) {
            buf[pos++] = ',';
            buf[pos++] = ' ';
        }
    }
    buf[pos] = '\0';
}

void panic_handler(registers_t* regs)
{
    __asm__ volatile("cli");
    char buf[64];

    uint32_t cr2 = 0;
    if (regs->int_no == 14)
        __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));

    terminal_setcolor(0x4F);
    terminal_initialize();
    terminal_writestring("  *** JEXOS PANIC ***  \n\n");

    const char* ex_name = "Unknown";
    if (regs->int_no < 32)
        ex_name = exception_names[regs->int_no];

    terminal_writestring("Exception: ");
    terminal_writestring(ex_name);
    terminal_writestring("  (int=");
    format_hex(regs->int_no, buf);
    terminal_writestring(buf);
    terminal_writestring("  err=");
    format_hex(regs->err_code, buf);
    terminal_writestring(buf);
    terminal_writestring(")\n");

    if (regs->int_no == 14) {
        terminal_writestring("Fault Addr: ");
        print_hex_val(cr2);
        terminal_writestring("  (CR2)\n");
        char decode[64];
        decode_page_fault_err(regs->err_code, decode, sizeof(decode));
        terminal_writestring("Error bits: ");
        terminal_writestring(decode);
        terminal_writestring("\n");
    }

    terminal_writestring("\nRegisters:\n");
    terminal_writestring("EAX: "); print_hex_val(regs->eax);
    terminal_writestring("  EBX: "); print_hex_val(regs->ebx);
    terminal_writestring("  ECX: "); print_hex_val(regs->ecx);
    terminal_writestring("  EDX: "); print_hex_val(regs->edx);
    terminal_writestring("\n");
    terminal_writestring("ESI: "); print_hex_val(regs->esi);
    terminal_writestring("  EDI: "); print_hex_val(regs->edi);
    terminal_writestring("  EBP: "); print_hex_val(regs->ebp);
    terminal_writestring("  ESP: "); print_hex_val(regs->esp);
    terminal_writestring("\n");
    terminal_writestring("EIP: "); print_hex_val(regs->eip);
    terminal_writestring("  EFLAGS: "); print_hex_val(regs->eflags);
    terminal_writestring("\n");

    /* CR0, CR3 */
    uint32_t cr0_val, cr3_val;
    __asm__ volatile("mov %%cr0, %0" : "=r"(cr0_val));
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3_val));
    terminal_writestring("CR0: "); print_hex_val(cr0_val);
    terminal_writestring("  CR2: "); print_hex_val(cr2);
    terminal_writestring("  CR3: "); print_hex_val(cr3_val);
    terminal_writestring("\n");

    /* Serial stack trace */
    dump_stack_serial();

    /* Detect stack overflow via guard page access */
    if (regs->int_no == 14) {
        uint32_t cr2;
        __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));
        log_serial("PAGE FAULT at 0x");
        log_hex_serial(cr2);
        log_serial(" (");
        if (regs->err_code & 0x1) log_serial("protection "); else log_serial("not-present ");
        if (regs->err_code & 0x2) log_serial("write "); else log_serial("read ");
        if (regs->err_code & 0x4) log_serial("user");
        log_serial(")\n");
    }

    /* Stack trace */
    uint32_t eip_frames[MAX_BACKTRACE_DEPTH];
    int depth = unwind_stack(regs->ebp, eip_frames, MAX_BACKTRACE_DEPTH);
    if (depth > 0) {
        terminal_writestring("\nStack Trace (depth ");
        char d[4];
        int n = depth, j = 0;
        if (n == 0) { d[j++] = '0'; }
        else { char tmp[8]; int k = 0;
            while (n > 0) { tmp[k++] = '0' + (n % 10); n /= 10; }
            while (k > 0) d[j++] = tmp[--k]; }
        d[j] = '\0';
        terminal_writestring(d);
        terminal_writestring("):\n");
        for (int i = 0; i < depth; i++) {
            terminal_writestring("  ");
            print_hex_val(eip_frames[i]);
            terminal_writestring("\n");
        }
    }

    /* Serial dump for AI agents */
    log_serial("*** JEXOS PANIC ***\n");
    log_serial("Exception: ");
    log_serial(ex_name);
    log_serial("\n");

    terminal_setcolor(0x07);
    terminal_writestring("\n\nSystem halted. [R]eboot, [D]ump crash log, [Enter] halt.\n");

    /* Interactive loop */
    while (1) {
        unsigned char key = 0;
        if (inb(0x64) & 0x01) {
            key = inb(0x60);
        } else {
            /* Can't hlt with IF=0; spin-wait */
            __asm__ volatile("pause");
            continue;
        }

        if (key == KEY_R) {
            log_serial("PANIC: user pressed R — rebooting\n");
            reboot();
        }
        if (key == KEY_D) {
            int fd = fs_open("/crashlog.hex", 0);
            if (fd < 0) {
                fs_create("/crashlog.hex");
                fd = fs_open("/crashlog.hex", 0);
            }
            if (fd >= 0) {
                fs_write(fd, "JEXOS PANIC DUMP\n", 17);
                fs_write(fd, "Exception: ", 11);
                fs_write(fd, ex_name, strlen(ex_name));
                fs_write(fd, "\n", 1);
                fs_write(fd, "EIP: ", 5);
                format_hex(regs->eip, buf);
                fs_write(fd, buf, 10);
                fs_write(fd, "\n", 1);
                fs_close(fd);
                log_serial("PANIC: crash log saved to /crashlog.hex\n");
                terminal_writestring("Crash log saved to /crashlog.hex\n");
            } else {
                terminal_writestring("Failed to save crash log\n");
            }
        }
        if (key == KEY_ENTER) {
            log_serial("PANIC: user pressed Enter — halting\n");
            break;
        }
    }

    while (1) { __asm__ volatile("hlt"); }
}

void panic_assert(const char* msg, const char* file, int line)
{
    __asm__ volatile("cli");
    terminal_setcolor(0x4F);
    terminal_initialize();
    terminal_writestring("  *** ASSERT FAILED ***  \n\n");
    terminal_writestring("Message: ");
    terminal_writestring(msg);
    terminal_writestring("\nFile: ");
    terminal_writestring(file);
    terminal_writestring("\nLine: ");
    char lbuf[16];
    int n = line, i = 0;
    if (n == 0) { lbuf[i++] = '0'; }
    else { char tmp[8]; int j = 0;
        while (n > 0) { tmp[j++] = '0' + (n % 10); n /= 10; }
        while (j > 0) lbuf[i++] = tmp[--j]; }
    lbuf[i] = '\0';
    terminal_writestring(lbuf);
    terminal_writestring("\n\nSystem Halted.\n");

    log_serial("ASSERT FAILED: ");
    log_serial(msg);
    log_serial(" at ");
    log_serial(file);
    log_serial(":");
    log_serial(lbuf);
    log_serial("\n");

    while (1) { __asm__ volatile("hlt"); }
}
