/**
 * @file gdb_stub.c
 * @brief Minimal GDB Remote Serial Protocol stub over COM1.
 *
 * Purpose: Provide a GDB stub so that a remote debugger can connect over
 *          the serial port (115200 bps, 8N1) and control kernel execution.
 *
 * Design:
 *  - Communicates via raw port I/O on COM1 (0x3F8) so the stub works even
 *    when higher-level kernel services are not fully initialised.
 *  - Supports the minimal GDB remote protocol subset needed for interactive
 *    debugging:   ? (stop-reason), g (read regs), m (mem read), M (mem write),
 *    c (continue), s (single-step), Z0/z0 (sw breakpoint insert/remove).
 *  - A shadow table holds the original byte for each software breakpoint so
 *    the stub can restore + re-arm the int3 instruction around single-steps.
 *
 * Thread-safety:  Only one CPU/core is assumed.  Stub re-enters via int3
 *                 (breakpoint) or int1 (single-step trace).  No locking.
 */

#include "debug/gdb_stub.h"
#include "ports.h"
#include <stddef.h>

/* ------------------------------------------------------------------
 * Serial I/O helpers — raw port access to COM1.
 * We do NOT use the kernel's serial.c because it can behave
 * unpredictably during a breakpoint (e.g. locks, incomplete init).
 * ------------------------------------------------------------------ */
#define COM1 0x3F8
#define COM1_LSR (COM1 + 5)   /* Line Status Register */
#define LSR_DR  0x01          /* Data Ready (receive) */
#define LSR_THRE 0x20         /* Transmitter Hold Register Empty */

static void gdb_putchar(char c)
{
    /* Wait for transmitter to be empty */
    while (!(inb(COM1_LSR) & LSR_THRE))
        ;
    outb(COM1, c);
}

static char gdb_getchar(void)
{
    /* Wait for data to arrive */
    while (!(inb(COM1_LSR) & LSR_DR))
        ;
    return inb(COM1);
}

/* ------------------------------------------------------------------
 * Packet framing (GDB Remote Serial Protocol)
 * ------------------------------------------------------------------ */

/* Packet buffer — single-shot; not re-entrant */
#define BUF_SIZE 512
static char pkt_buf[BUF_SIZE];

/*
 * Send a packet:  $<data>#<2-hex-checksum>
 * The checksum is the 8-bit sum of all data bytes.
 */
static void gdb_send_packet(const char *data, int len)
{
    uint8_t csum = 0;
    int i;

    gdb_putchar('$');
    for (i = 0; i < len; i++) {
        gdb_putchar(data[i]);
        csum += (uint8_t)data[i];
    }
    gdb_putchar('#');

    const char *hex = "0123456789abcdef";
    gdb_putchar(hex[(csum >> 4) & 0xF]);
    gdb_putchar(hex[csum & 0xF]);
}

/*
 * Receive a packet (blocking).
 * Skips '+' ACK and resyncs on '$'.
 * Returns the payload length (0 on error).
 * Always sends an ACK ('+') — the stub does not validate checksums.
 */
static int gdb_recv_packet(char *buf)
{
    /* Wait for start of packet */
    for (;;) {
        char c = gdb_getchar();
        if (c == '+')          /* ACK from previous response — ignore */
            continue;
        if (c == '$')          /* Start of packet */
            break;
        /* Anything else is ignored (resync) */
    }

    int pos = 0;
    for (;;) {
        char c = gdb_getchar();
        if (c == '#')
            break;
        if (c == '$') {        /* Resync on spurious start-of-packet */
            pos = 0;
            continue;
        }
        if (pos < BUF_SIZE - 1)
            buf[pos++] = c;
    }

    /* Read two hex checksum digits (acknowledge but do not verify) */
    (void)gdb_getchar();
    (void)gdb_getchar();

    gdb_putchar('+');          /* ACK */
    return pos;
}

/* ------------------------------------------------------------------
 * Hex conversion helpers
 * ------------------------------------------------------------------ */

static char nibble_hex(uint8_t v)
{
    return "0123456789abcdef"[v & 0xF];
}

static int hex_to_byte(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return 0;
}

/* ------------------------------------------------------------------
 * Memory helpers (raw read/write, GDB hex encoding)
 * ------------------------------------------------------------------ */

/*
 * mem_to_hex - Read @len bytes from @addr, write as hex to @out.
 * Returns number of hex characters written (len * 2).
 */
static int mem_to_hex(char *out, uint32_t addr, int len)
{
    int i;
    for (i = 0; i < len; i++) {
        uint8_t b = *(volatile uint8_t *)addr;
        out[i * 2]     = nibble_hex(b >> 4);
        out[i * 2 + 1] = nibble_hex(b);
        addr++;
    }
    return len * 2;
}

/*
 * hex_to_mem - Write @hex_len/2 bytes parsed from hex string to @addr.
 * Returns number of bytes written.
 */
static int hex_to_mem(uint32_t addr, const char *hex, int hex_len)
{
    int i;
    for (i = 0; i < hex_len / 2; i++) {
        uint8_t v = 0;
        int j;
        for (j = 0; j < 2; j++) {
            char c = hex[i * 2 + j];
            v <<= 4;
            if (c >= '0' && c <= '9')      v |= c - '0';
            else if (c >= 'a' && c <= 'f') v |= c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') v |= c - 'A' + 10;
        }
        *(volatile uint8_t *)addr = v;
        addr++;
    }
    return hex_len / 2;
}

/* ------------------------------------------------------------------
 * Register-file helpers
 * ------------------------------------------------------------------ */

/*
 * regs_to_hex - Serialise the 16 GDB i386 registers as a hex string.
 *
 * GDB register order for i386:
 *   EAX, ECX, EDX, EBX, ESP, EBP, ESI, EDI,
 *   EIP, EFLAGS, CS, SS, DS, ES, FS, GS
 *
 * Each register is 4 bytes, big-endian in the hex stream.
 */
static int regs_to_hex(char *out, registers_t *regs)
{
    uint32_t r[] = {
        regs->eax, regs->ecx, regs->edx, regs->ebx,
        regs->esp, regs->ebp, regs->esi, regs->edi,
        regs->eip, regs->eflags,
        0x08, 0x10, 0x10, 0x10, 0x10, 0x10
    };
    int pos = 0;
    int i;
    for (i = 0; i < 16; i++) {
        int b;
        for (b = 3; b >= 0; b--) {
            out[pos++] = nibble_hex(r[i] >> (b * 4 + 4));
            out[pos++] = nibble_hex(r[i] >> (b * 4));
        }
    }
    return pos;   /* 16 regs * 8 hex chars = 128 */
}

/* ------------------------------------------------------------------
 * Software breakpoint shadow table
 * ------------------------------------------------------------------ */

#define MAX_BP 256
static uint8_t  bp_orig[MAX_BP];   /* original byte at each BP address */
static uint32_t bp_addr[MAX_BP];   /* BP addresses */
static int      bp_count;          /* current number of BPs */

/*
 * stepping_over - Flag set by the 'c' (continue) command so that the
 * single-step trace handler (int 1) knows to silently continue rather
 * than re-entering the GDB stub loop.
 */
static int stepping_over;

/* ------------------------------------------------------------------
 * Trace handler — invoked from the int 1 (debug exception) path.
 * ------------------------------------------------------------------ */

void gdb_stub_handle_trace(registers_t *regs)
{
    regs->eflags &= ~0x100;   /* Clear Trap Flag */

    if (stepping_over) {
        /* Silently stepped past a breakpoint — continue. */
        stepping_over = 0;
        return;
    }

    /* User-requested single-step — enter stub command loop. */
    gdb_stub_handler(regs);
}

/* ------------------------------------------------------------------
 * Main handler — called for both int3 (breakpoint) and int1 (single-step).
 * ------------------------------------------------------------------ */

void gdb_stub_handler(registers_t *regs)
{
    /*
     * Step 1 — Restore the byte we replaced with int3 so the CPU can
     * execute the original instruction.  eip must be rewound by 1
     * because the CPU advanced past the 1-byte int3 (0xCC) already.
     */
    uint32_t bp_eip = regs->eip - 1;
    int i;
    for (i = 0; i < bp_count; i++) {
        if (bp_addr[i] == bp_eip) {
            *(volatile uint8_t *)bp_eip = bp_orig[i];
            regs->eip = bp_eip;
            break;
        }
    }

    /*
     * Step 2 — Notify GDB that a debug event occurred even before
     * we process commands (some GDB versions wait for a stop reply).
     *
     * Send '+' to let GDB know the channel is alive.
     */
    gdb_putchar('+');

    /*
     * Step 3 — Command loop.
     */
    for (;;) {
        int len = gdb_recv_packet(pkt_buf);
        if (len <= 0)
            continue;

        char cmd = pkt_buf[0];
        char reply[1024];
        int  rlen = 0;

        switch (cmd) {

        /* ---- ? — Stop-reason query (SIGTRAP) ---- */
        case '?':
            reply[0] = 'S';
            reply[1] = '0';
            reply[2] = '5';
            rlen = 3;
            break;

        /* ---- g — Read general registers ---- */
        case 'g':
            rlen = regs_to_hex(reply, regs);
            break;

        /* ---- G — Write general registers (not implemented) ---- */
        case 'G':
            reply[0] = 'E'; reply[1] = '0'; reply[2] = '1';
            rlen = 3;
            break;

        /* ---- m <addr>,<len> — Read memory ---- */
        case 'm': {
            uint32_t addr = 0, rlen2 = 0;
            int pos = 1;
            while (pos < len &&
                   ((pkt_buf[pos] >= '0' && pkt_buf[pos] <= '9') ||
                    (pkt_buf[pos] >= 'a' && pkt_buf[pos] <= 'f'))) {
                addr = (addr << 4) + hex_to_byte(pkt_buf[pos]);
                pos++;
            }
            pos++;   /* skip comma */
            while (pos < len &&
                   ((pkt_buf[pos] >= '0' && pkt_buf[pos] <= '9') ||
                    (pkt_buf[pos] >= 'a' && pkt_buf[pos] <= 'f'))) {
                rlen2 = (rlen2 << 4) + hex_to_byte(pkt_buf[pos]);
                pos++;
            }
            rlen = mem_to_hex(reply, addr, (int)rlen2);
            break;
        }

        /* ---- M <addr>,<len>:<data> — Write memory ---- */
        case 'M': {
            uint32_t addr = 0;
            int pos = 1;
            while (pkt_buf[pos] && pkt_buf[pos] != ',' &&
                   ((pkt_buf[pos] >= '0' && pkt_buf[pos] <= '9') ||
                    (pkt_buf[pos] >= 'a' && pkt_buf[pos] <= 'f'))) {
                addr = (addr << 4) + hex_to_byte(pkt_buf[pos]);
                pos++;
            }
            pos++;   /* skip comma */
            uint32_t wlen = 0;
            while (pkt_buf[pos] && pkt_buf[pos] != ':') {
                wlen = (wlen << 4) + hex_to_byte(pkt_buf[pos]);
                pos++;
            }
            pos++;   /* skip colon */
            hex_to_mem(addr, &pkt_buf[pos], (int)wlen * 2);
            reply[0] = 'O'; reply[1] = 'K';
            rlen = 2;
            break;
        }

        /* ---- c — Continue ---- */
        case 'c':
            /* Re-arm all breakpoints before returning. */
            for (i = 0; i < bp_count; i++)
                *(volatile uint8_t *)bp_addr[i] = 0xCC;
            /*
             * Set the stepping flag so the int1 trace handler knows
             * to silently continue after the breakpoint step.
             */
            stepping_over = 1;
            regs->eflags |= 0x100;   /* set Trap Flag */
            return;

        /* ---- s — Single-step ---- */
        case 's':
            stepping_over = 0;
            regs->eflags |= 0x100;   /* set Trap Flag */
            return;

        /* ---- Z0,<addr>,<kind> — Insert sw breakpoint ---- */
        case 'Z': {
            uint32_t addr = 0;
            int pos = 2;             /* skip 'Z' and type '0' */
            if (pkt_buf[pos] == ',')
                pos++;               /* skip comma */
            while (pkt_buf[pos] && pkt_buf[pos] != ',' &&
                   ((pkt_buf[pos] >= '0' && pkt_buf[pos] <= '9') ||
                    (pkt_buf[pos] >= 'a' && pkt_buf[pos] <= 'f'))) {
                addr = (addr << 4) + hex_to_byte(pkt_buf[pos]);
                pos++;
            }
            if (bp_count < MAX_BP) {
                bp_addr[bp_count] = addr;
                bp_orig[bp_count] = *(volatile uint8_t *)addr;
                *(volatile uint8_t *)addr = 0xCC;
                bp_count++;
            }
            reply[0] = 'O'; reply[1] = 'K';
            rlen = 2;
            break;
        }

        /* ---- z0,<addr>,<kind> — Remove sw breakpoint ---- */
        case 'z': {
            uint32_t addr = 0;
            int pos = 2;             /* skip 'z' and type '0' */
            if (pkt_buf[pos] == ',')
                pos++;               /* skip comma */
            while (pkt_buf[pos] && pkt_buf[pos] != ',' &&
                   ((pkt_buf[pos] >= '0' && pkt_buf[pos] <= '9') ||
                    (pkt_buf[pos] >= 'a' && pkt_buf[pos] <= 'f'))) {
                addr = (addr << 4) + hex_to_byte(pkt_buf[pos]);
                pos++;
            }
            for (i = 0; i < bp_count; i++) {
                if (bp_addr[i] == addr) {
                    *(volatile uint8_t *)addr = bp_orig[i];
                    bp_addr[i] = bp_addr[bp_count - 1];
                    bp_orig[i] = bp_orig[bp_count - 1];
                    bp_count--;
                    break;
                }
            }
            reply[0] = 'O'; reply[1] = 'K';
            rlen = 2;
            break;
        }

        /* ---- Unrecognised command — empty reply (GDB re-sends) ---- */
        default:
            rlen = 0;
            break;
        }

        if (rlen > 0)
            gdb_send_packet(reply, rlen);
    }
}
