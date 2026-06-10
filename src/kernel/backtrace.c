/**
 * @file backtrace.c
 * @brief Stack unwinder and backtrace display.
 *
 * Walks the EBP-linked call chain to produce human-readable stack traces
 * for debugging kernel panics and shell diagnostics.
 *
 * Thread-safety: Uses inline asm to read the current EBP, which is
 * per-CPU. Not safe to call concurrently on the same CPU.
 */

#include "kernel/backtrace.h"
#include "terminal.h"
#include "serial.h"

void format_hex(uint32_t val, char* out)
{
    const char* digits = "0123456789ABCDEF";
    out[0] = '0'; out[1] = 'x';
    for (int i = 0; i < 8; i++)
        out[2 + i] = digits[(val >> (28 - i * 4)) & 0xF];
    out[10] = '\0';
}

int unwind_stack(uint32_t ebp, uint32_t* eip_out, int max_frames)
{
    int count = 0;
    while (ebp != 0 && eip_out && count < max_frames) {
        if ((ebp & 3) != 0 || ebp < 0x100000)
            break;
        uint32_t* frame = (uint32_t*)ebp;
        uint32_t saved_ebp = frame[0];
        uint32_t saved_eip = frame[1];
        if (saved_ebp <= ebp || saved_ebp > 0xFFFFF000)
            break;
        eip_out[count++] = saved_eip;
        ebp = saved_ebp;
    }
    return count;
}

static void dump_stack_common(int to_terminal)
{
    uint32_t ebp;
    __asm__ volatile("mov %%ebp, %0" : "=r"(ebp));
    uint32_t eip_frames[MAX_BACKTRACE_DEPTH];
    int depth = unwind_stack(ebp, eip_frames, MAX_BACKTRACE_DEPTH);
    char buf[12];

    for (int i = 0; i < depth; i++) {
        log_serial("  [<");
        log_hex_serial(eip_frames[i]);
        log_serial(">]\n");
        if (to_terminal) {
            terminal_writestring("  [<");
            format_hex(eip_frames[i], buf);
            terminal_writestring(buf);
            terminal_writestring(">]\n");
        }
    }
}

void dump_stack(void)
{
    terminal_writestring("Call stack:\n");
    log_serial("Call stack:\n");
    dump_stack_common(1);
}

void dump_stack_serial(void)
{
    log_serial("Call stack:\n");
    dump_stack_common(0);
}
