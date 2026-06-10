/**
 * @file dump.c
 * @brief Hexdump memory utility — prints memory regions to the terminal.
 *
 * Purpose: Shell command for inspecting kernel memory at runtime.
 *          Reads directly from virtual addresses; if the address is
 *          unmapped, the page fault handler will catch it.
 */

#include "dump.h"
#include "terminal.h"
#include "panic.h"
#include <string.h>

/* Forward: int_to_string is defined in shell.c */
extern void int_to_string(int n, char* str);

/**
 * @brief Hexdump 'len' bytes starting at 'addr'.
 *
 * Prints address, hex bytes (16 per line, with gap at byte 8),
 * and ASCII representation. Uses format_hex for addresses and
 * int_to_string for lengths.
 *
 * @param addr  Starting virtual address.
 * @param len   Number of bytes to dump (clamped 0–1024, default 128).
 */
void hexdump(uint32_t addr, uint32_t len)
{
    const char* hex = "0123456789ABCDEF";
    char buf[80];

    if (len > 1024)
        len = 1024;
    if (len == 0)
        len = 128;

    uint8_t* ptr = (uint8_t*)(uint32_t)addr;

    terminal_writestring("Hexdump of ");
    format_hex(addr, buf);
    terminal_writestring(buf);
    terminal_writestring(" (");
    int_to_string((int)len, buf);
    terminal_writestring(buf);
    terminal_writestring(" bytes):\n");

    for (uint32_t i = 0; i < len; i += 16) {
        int pos = 0;

        /* Address */
        format_hex(addr + i, buf + pos);
        pos = 10;
        buf[pos++] = ' ';

        /* Hex bytes */
        for (uint32_t j = 0; j < 16; j++) {
            if (i + j < len) {
                uint8_t b = ptr[i + j];
                buf[pos++] = hex[(b >> 4) & 0xF];
                buf[pos++] = hex[b & 0xF];
            } else {
                buf[pos++] = ' ';
                buf[pos++] = ' ';
            }
            buf[pos++] = ' ';
            if (j == 7)
                buf[pos++] = ' ';
        }

        buf[pos++] = ' ';

        /* ASCII */
        for (uint32_t j = 0; j < 16 && i + j < len; j++) {
            uint8_t b = ptr[i + j];
            buf[pos++] = (b >= 32 && b < 127) ? (char)b : '.';
        }

        buf[pos++] = '\n';
        buf[pos] = '\0';
        terminal_writestring(buf);
    }
}
