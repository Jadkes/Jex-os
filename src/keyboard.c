#include "keyboard.h"
#include "irq.h"
#include "ports.h"
#include "shell.h"
#include <stdint.h>
#include <stdbool.h>

extern void terminal_putchar(char c);
extern void terminal_writestring(const char* data);

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

bool shift_held = false;
bool ctrl_held = false;

void keyboard_callback(registers_t *regs) {
    uint8_t scancode = inb(0x60);
    
    if (scancode == 0xE0) {
        // Extended code, ignore for now or handle next byte
        return; 
    }

    if (scancode & 0x80) {
        // Key release
        uint8_t released_code = scancode & 0x7F;
        if (released_code == 42 || released_code == 54) shift_held = false;
        if (released_code == 29) ctrl_held = false;
    } else {
        // Key press
        if (scancode == 42 || scancode == 54) { shift_held = true; return; }
        if (scancode == 29) { ctrl_held = true; return; }

        char c = shift_held ? kbdus_shifted[scancode] : kbdus[scancode];
        
        // Handle Ctrl combinations
        if (ctrl_held) {
            if (c == 's' || c == 'S') c = 0x13; // Ctrl+S (Save)
            else if (c == 'q' || c == 'Q') c = 0x11; // Ctrl+Q (Quit)
            else if (c == 'b' || c == 'B') c = 0x02; // Ctrl+B (Build)
        }

        if (c != 0) {
            shell_input(c);
        }
    }
    (void)regs;
}

void init_keyboard() {
    register_interrupt_handler(1, keyboard_callback);
}