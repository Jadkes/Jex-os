/**
 * @file keyboard.h
 * @brief PS/2 Keyboard driver interface.
 *
 * Handles keyboard input interrupts and scancode translation.
 */

#ifndef KEYBOARD_H
#define KEYBOARD_H

#define KBD_BUF_SIZE 256

/* Set-1 scancode markers */
#define KBD_RELEASE_BIT      0x80    /* break code flag            */
#define KBD_EXT_PREFIX       0xE0    /* extended-key prefix byte   */

/* Pseudo-ASCII values the tables emit for extended keys.
 * Consumers (shell, editor) match on these. */
#define KEY_HOME             0x84
#define KEY_UP               0x80
#define KEY_PAGE_UP          0x85
#define KEY_LEFT             0x82
#define KEY_RIGHT            0x83
#define KEY_END              0x86
#define KEY_DOWN             0x81
#define KEY_PAGE_DOWN        0x87

/* Scancodes for modifier keys */
#define SCAN_LEFT_SHIFT      42
#define SCAN_RIGHT_SHIFT     54
#define SCAN_CTRL            29

/* ASCII control-code shorthand for Ctrl+<key> shortcuts */
#define CTRL(ch)             ((ch) & 0x1F)

/**
 * @brief Initialize the keyboard driver and register its interrupt handler.
 */
void init_keyboard();

/**
 * @brief Read the next character from the keyboard ring buffer (non-blocking).
 * @return The character, or -1 if the buffer is empty.
 */
int keyboard_read(void);

/**
 * @brief Check if the keyboard buffer has data (non-blocking).
 * @return 1 if data available, 0 otherwise.
 */
int keyboard_has_data(void);

/**
 * @brief Flush/discard all pending keyboard input.
 */
void keyboard_flush(void);

#endif // KEYBOARD_H
