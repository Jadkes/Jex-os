/**
 * @file keyboard.c
 * @brief PS/2 Keyboard driver.
 *
 * This file handles keyboard interrupts (IRQ1), translates scancodes into ASCII,
 * manages modifier keys (Shift, Ctrl), and sends input to the shell.
 */

#include "keyboard.h"
#include "kernel/atomic.h"
#include "ringbuf.h"
#include "spinlock.h"
#include "irq.h"
#include "init.h"
#include "ports.h"
#include "task.h"
#include <stdint.h>
#include <stdbool.h>

/* Forward declarations for terminal functions used for direct output if needed */
extern void terminal_putchar(char c);
extern void terminal_writestring(const char* data);

/**
 * @brief US Keyboard Layout scancode mapping (Lowercase/Normal).
 * Maps a single-byte scancode to its ASCII equivalent.
 */
unsigned char kbdus[128] = {
    0,  27, '1', '2', '3', '4', '5', '6', '7', '8',	/* 9 */
    '9', '0', '-', '=', '\b',	/* Backspace */
    '\t',		/* Tab */
    'q', 'w', 'e', 'r',	/* 19 */
    't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n',	/* Enter key */
    0,		/* 29   - Control */
    'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';',	/* 39 */
    '\'', '`',   0,		/* Left shift */
    '\\', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/',   0,				/* Right shift */
    '*',
    0,	/* Alt */
    ' ',	/* Space bar */
    0,	/* Caps lock */
    0,	/* 59 - F1 key ... > */
    0,   0,   0,   0,   0,   0,   0,   0,
    0,	/* < ... F10 */
    0,	/* 69 - Num lock*/
    0,	/* Scroll Lock */
    0x84,	/* Home key */
    KEY_UP,
    0x85,	/* Page Up */
    '-',
    KEY_LEFT,
    0,
    KEY_RIGHT,
    '+',
    0x86,	/* End key*/
    KEY_DOWN,
    0x87,	/* Page Down */
    0,	/* Insert Key */
    0,	/* Delete Key */
    0,   0,   0,
    0,	/* F11 Key */
    0,	/* F12 Key */
    0	/* All other keys are undefined */
};

/**
 * @brief US Keyboard Layout scancode mapping (Uppercase/Shifted).
 */
unsigned char kbdus_shifted[128] = {
    0,  27, '!', '@', '#', '$', '%', '^', '&', '*',	/* 9 */
    '(', ')', '_', '+', '\b',	/* Backspace */
    '\t',		/* Tab */
    'Q', 'W', 'E', 'R',	/* 19 */
    'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n',	/* Enter key */
    0,		/* 29   - Control */
    'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':',	/* 39 */
    '"', '~',   0,		/* Left shift */
    '|', 'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?',   0,				/* Right shift */
    '*',
    0,	/* Alt */
    ' ',	/* Space bar */
    0,	/* Caps lock */
    0,	/* 59 - F1 key ... > */
    0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,
    KEY_HOME, KEY_UP, KEY_PAGE_UP, '-', KEY_LEFT, 0, KEY_RIGHT, '+',
    KEY_END, KEY_DOWN, KEY_PAGE_DOWN,
    0, 0, 0, 0, 0, 0
};

/* State flags for modifier keys */
bool shift_held = false;
bool ctrl_held = false;

/* Keyboard ring buffer — ISR produces, shell consumes (SPSC, no locks) */
static ringbuf_t kbd_ring;
static volatile uint8_t kbd_storage[KBD_BUF_SIZE];

/*
 * flush() moves both indices, so it needs the IRQ mask rather than the
 * SPSC single-owner rule.  A plain cli/sti pair would clobber a caller
 * that already had interrupts off; irq_save captures the real state.
 */
static spinlock_t kbd_ring_lock = SPINLOCK_INIT;

/**
 * @brief Keyboard Interrupt Callback (IRQ1).
 * Reads the scancode from port 0x60 and processes key presses/releases.
 * 
 * @param regs CPU registers state.
 */
void keyboard_callback(registers_t *regs) {
    uint8_t scancode = inb(KBD_DATA_PORT);

    if (scancode == KBD_EXT_PREFIX)
        return;

    if (scancode & KBD_RELEASE_BIT) {
        /* Break code: track modifier releases */
        uint8_t released_code = scancode & ~KBD_RELEASE_BIT;
        if (released_code == SCAN_LEFT_SHIFT || released_code == SCAN_RIGHT_SHIFT) shift_held = false;
        if (released_code == SCAN_CTRL) ctrl_held = false;
    } else {
        /* Make code */
        if (scancode >= 128) return;  /* Defensive: OOB guard for kbdus[] */
        if (scancode == SCAN_LEFT_SHIFT || scancode == SCAN_RIGHT_SHIFT) { shift_held = true; return; }
        if (scancode == SCAN_CTRL) { ctrl_held = true; return; }

        char c = shift_held ? kbdus_shifted[scancode] : kbdus[scancode];

        /* Handle Ctrl combinations for shell/editor shortcuts */
        if (ctrl_held) {
            if (c == 's' || c == 'S') c = CTRL('S');
            else if (c == 'q' || c == 'Q') c = CTRL('Q');
            else if (c == 'b' || c == 'B') c = CTRL('B');
            else if (c == 'v' || c == 'V') c = CTRL('V');
            else if (c == 'a' || c == 'A') c = CTRL('A');
            else if (c == 'c' || c == 'C') c = CTRL('C');
            else if (c == 'f' || c == 'F') c = CTRL('F');
            else if (c == 'g' || c == 'G') c = CTRL('G');
            else if (c == 'n' || c == 'N') c = CTRL('N');
            else if (c == 'x' || c == 'X') c = CTRL('X');
            else if (c == 'z' || c == 'Z') c = CTRL('Z');
        }

        /* Ctrl+C while a foreground child runs delivers SIGINT instead of
         * buffering the keystroke — the shell is parked in waitpid() and
         * would not consume it until the child exits anyway. */
        if (c == CTRL('C') && fg_child_pid > 0) {
            task_t* fg = task_from_pid(fg_child_pid);
            if (fg) {
                atomic_or(&fg->signal_pending, 1u << SIGINT);
                terminal_writestring("^C\n");
                return;
            }
        }

        if (c != 0)
            ringbuf_put(&kbd_ring, (uint8_t)c);  /* full -> dropped */
    }
    (void)regs;
}

/**
 * @brief Initialize the keyboard driver.
 * Registers the keyboard_callback as the handler for IRQ1.
 */
void init_keyboard() {
    ringbuf_init(&kbd_ring, kbd_storage, KBD_BUF_SIZE);
    register_interrupt_handler(1, keyboard_callback);
}

/**
 * @brief Read the next character from the keyboard ring buffer (non-blocking).
 * @return The character (0-255), or -1 if buffer is empty.
 */
int keyboard_read(void)
{
    return ringbuf_get(&kbd_ring);
}

/**
 * @brief Check if keyboard data is available (non-blocking).
 * @return 1 if data available, 0 otherwise.
 */
int keyboard_has_data(void)
{
    return ringbuf_has_data(&kbd_ring);
}

/**
 * @brief Flush all pending keyboard input by resetting the buffer.
 *
 * Disables interrupts to prevent the ISR from modifying head/tail
 * between the two writes — without this, a keypress in the ISR could
 * set head non-zero after we reset it, corrupting the ring buffer.
 */
void keyboard_flush(void)
{
    uint32_t flags;
    spin_lock_irqsave(&kbd_ring_lock, &flags);
    ringbuf_reset(&kbd_ring);
    spin_unlock_irqrestore(&kbd_ring_lock, flags);
}

early_init(init_keyboard);
