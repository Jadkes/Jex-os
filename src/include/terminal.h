/**
 * @file terminal.h
 * @brief VGA text mode output helpers.
 *
 * Provides functions for writing text to the screen, controlling the cursor,
 * and managing terminal state.
 */

#ifndef TERMINAL_H
#define TERMINAL_H

#include <stddef.h>
#include <stdint.h>

/**
 * @brief Initialize the VGA text terminal.
 * Clears the screen and resets the cursor to (0,0).
 */
void terminal_initialize(void);

/**
 * @brief Set the default text color for subsequent output.
 * @param color VGA attribute byte (foreground/background).
 */
void terminal_setcolor(uint8_t color);

/**
 * @brief Write a single character at the current cursor position.
 * Automatically handles newlines and scrolling.
 */
void terminal_putchar(char c);

/**
 * @brief Write a null-terminated string to the display.
 */
void terminal_writestring(const char* data);

/**
 * @brief Write raw data of a fixed size to the display.
 */
void terminal_write(const char* data, size_t size);

/**
 * @brief Hardware cursor control helpers.
 */
void enable_cursor(uint8_t cursor_start, uint8_t cursor_end);
void update_cursor(int x, int y);

/**
 * @brief Place a character at a specific location on the screen.
 */
void terminal_putentryat(char c, uint8_t color, size_t x, size_t y);

/**
 * @brief Shared state: current terminal row.
 */
extern size_t terminal_row;

#endif // TERMINAL_H
