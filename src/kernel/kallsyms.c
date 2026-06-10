/**
 * @file kallsyms.c
 * @brief Packed sorted symbol table for symbolic backtraces.
 *
 * The symbol table is built by gen_kallsyms from nm output and embedded
 * in the kernel via a two-pass link process. dump_stack uses this to
 * show function_name+offset/size instead of bare hex addresses.
 *
 * Layout in memory (at __kallsyms_start):
 *   [count: u32][entry0 addr: u32][entry0 name_off+size: u32]...
 *   ...entries...[string0\0][string1\0]...
 *
 * Thread-safety: Read-only after init; safe to call from any context.
 */

#include "kernel/kallsyms.h"
#include "init.h"
#include <string.h>

extern char __kallsyms_start[];
extern char __kallsyms_end[];

static kallsym_entry_t*   sym_entries;
static const char*        sym_strings;
static int                sym_count;

void kallsyms_init(void)
{
    uint32_t* header = (uint32_t*)__kallsyms_start;
    sym_count    = (int)header[0];
    sym_entries  = (kallsym_entry_t*)&header[1];
    sym_strings  = (const char*)&sym_entries[sym_count];
}

device_init(kallsyms_init);

const char* kallsyms_lookup(uint32_t addr, uint32_t* offset, uint32_t* size)
{
    int lo = 0, hi = sym_count - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        uint32_t func_addr = sym_entries[mid].addr;
        uint32_t func_size = (uint32_t)sym_entries[mid].size * 16;

        if (addr >= func_addr && addr < func_addr + func_size) {
            *offset = addr - func_addr;
            *size   = func_size;
            return &sym_strings[sym_entries[mid].name_off];
        }

        if (addr < func_addr)
            hi = mid - 1;
        else
            lo = mid + 1;
    }

    *offset = 0;
    *size   = 0;
    return "Unknown";
}
