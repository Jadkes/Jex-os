/**
 * @file keyboard.c
 * @brief PS/2 Keyboard driver.
 *
 * This file handles keyboard interrupts (IRQ1), translates scancodes into ASCII,
 * manages modifier keys (Shift, Ctrl), and sends input to the shell.
 */

#include "keyboard.h"
#include "irq.h"
#include "init.h"
#include "ports.h"
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
    0x80,	/* Up Arrow */
    0x85,	/* Page Up */
    '-',
    0x82,	/* Left Arrow */
    0,
    0x83,	/* Right Arrow */
    '+',
    0x86,	/* End key*/
    0x81,	/* Down Arrow */
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
    0x84, 0x80, 0x85, '-', 0x82, 0, 0x83, '+', 0x86, 0x81, 0x87,
    0, 0, 0, 0, 0, 0
};

/* State flags for modifier keys */
bool shift_held = false;
bool ctrl_held = false;

/* Keyboard ring buffer — ISR writes, shell reads */
static volatile char kbd_buffer[KBD_BUF_SIZE];
static volatile uint16_t kbd_head = 0;   /* Written by ISR */
static volatile uint16_t kbd_tail = 0;   /* Written by shell consumer */

/**
 * @brief Keyboard Interrupt Callback (IRQ1).
 * Reads the scancode from port 0x60 and processes key presses/releases.
 * 
 * @param regs CPU registers state.
 */
void keyboard_callback(registers_t *regs) {
    uint8_t scancode = inb(0x60);
    
    if (scancode == 0xE0) {
        /* Extended scancode prefix - ignored for now */
        return; 
    }

    if (scancode & 0x80) {
        /* Key release (Break code) */
        uint8_t released_code = scancode & 0x7F;
        if (released_code == 42 || released_code == 54) shift_held = false;
        if (released_code == 29) ctrl_held = false;
    } else {
        /* Key press (Make code) */
        if (scancode >= 128) return;  /* Defensive: OOB guard for kbdus[] */
        if (scancode == 42 || scancode == 54) { shift_held = true; return; }
        if (scancode == 29) { ctrl_held = true; return; }

        char c = shift_held ? kbdus_shifted[scancode] : kbdus[scancode];
        
        /* Handle Ctrl combinations for shell/editor shortcuts */
        if (ctrl_held) {
            if (c == 's' || c == 'S') c = 0x13;      /* Ctrl+S (Save) */
            else if (c == 'q' || c == 'Q') c = 0x11; /* Ctrl+Q (Quit) */
            else if (c == 'b' || c == 'B') c = 0x02; /* Ctrl+B (Build/Compile) */
            else if (c == 'v' || c == 'V') c = 0x16; /* Ctrl+V (Paste) */
            else if (c == 'a' || c == 'A') c = 0x01; /* Ctrl+A (Select All / About) */
            else if (c == 'c' || c == 'C') c = 0x03; /* Ctrl+C (Copy) */
            else if (c == 'f' || c == 'F') c = 0x06; /* Ctrl+F (Find) */
            else if (c == 'g' || c == 'G') c = 0x07; /* Ctrl+G (Goto Line) */
            else if (c == 'n' || c == 'N') c = 0x0E; /* Ctrl+N (New File) */
            else if (c == 'x' || c == 'X') c = 0x18; /* Ctrl+X (Cut) */
            else if (c == 'z' || c == 'Z') c = 0x1A; /* Ctrl+Z (Undo) */
        }

        if (c != 0) {
            /* Write to ring buffer (ISR producer, shell consumer) */
            uint16_t next_head = (kbd_head + 1) % KBD_BUF_SIZE;
            if (next_head != kbd_tail) {  /* Buffer not full */
                kbd_buffer[kbd_head] = c;
                kbd_head = next_head;
            }
            /* If buffer full, character is dropped silently */
        }
    }
    (void)regs;
}

/**
 * @brief Initialize the keyboard driver.
 * Registers the keyboard_callback as the handler for IRQ1.
 */
void init_keyboard() {
    register_interrupt_handler(1, keyboard_callback);
}

/**
 * @brief Read the next character from the keyboard ring buffer (non-blocking).
 * @return The character (0-255), or -1 if buffer is empty.
 */
int keyboard_read(void)
{
    if (kbd_head == kbd_tail) return -1;  /* Empty */
    char c = kbd_buffer[kbd_tail];
    kbd_tail = (kbd_tail + 1) % KBD_BUF_SIZE;
    return c;
}

/**
 * @brief Check if keyboard data is available (non-blocking).
 * @return 1 if data available, 0 otherwise.
 */
int keyboard_has_data(void)
{
    return (kbd_head != kbd_tail) ? 1 : 0;
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
    __asm__ volatile("cli");
    kbd_head = 0;
    kbd_tail = 0;
    __asm__ volatile("sti");
}

early_init(init_keyboard);
