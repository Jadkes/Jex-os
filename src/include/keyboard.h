/**
 * @file keyboard.h
 * @brief PS/2 Keyboard driver interface.
 *
 * Handles keyboard input interrupts and scancode translation.
 */

#ifndef KEYBOARD_H
#define KEYBOARD_H

/**
 * @brief Initialize the keyboard driver and register its interrupt handler.
 */
void init_keyboard();

#endif // KEYBOARD_H
