/**
 * @file keyboard.h
 * @brief PS/2 Keyboard driver interface.
 *
 * Handles keyboard input interrupts and scancode translation.
 */

#ifndef KEYBOARD_H
#define KEYBOARD_H

#define KBD_BUF_SIZE 256

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
