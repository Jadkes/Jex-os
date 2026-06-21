#include <terminal.h>
#include <ports.h>
#include <config.h>
#include <string.h>
#include <stddef.h>

/**
 * @file terminal.c
 * @brief VGA text mode driver.
 * Handles screen output, scrolling, and cursor management.
 */

// VGA colors
enum vga_color {
	VGA_COLOR_BLACK = 0,
	VGA_COLOR_BLUE = 1,
	VGA_COLOR_GREEN = 2,
	VGA_COLOR_CYAN = 3,
	VGA_COLOR_RED = 4,
	VGA_COLOR_MAGENTA = 5,
	VGA_COLOR_BROWN = 6,
	VGA_COLOR_LIGHT_GREY = 7,
	VGA_COLOR_DARK_GREY = 8,
	VGA_COLOR_LIGHT_BLUE = 9,
	VGA_COLOR_LIGHT_GREEN = 10,
	VGA_COLOR_LIGHT_CYAN = 11,
	VGA_COLOR_LIGHT_RED = 12,
	VGA_COLOR_LIGHT_MAGENTA = 13,
	VGA_COLOR_LIGHT_BROWN = 14,
	VGA_COLOR_WHITE = 15,
};

static inline uint8_t vga_entry_color(enum vga_color fg, enum vga_color bg) {
    return fg | bg << 4;
}

static inline uint16_t vga_entry(unsigned char uc, uint8_t color) {
    return (uint16_t) uc | (uint16_t) color << 8;
}

size_t terminal_row;
size_t terminal_column;
uint8_t terminal_color;
uint16_t* terminal_buffer;

/**
 * @brief Enables the VGA hardware cursor.
 * @param cursor_start The starting scanline of the cursor.
 * @param cursor_end The ending scanline of the cursor.
 */
void enable_cursor(uint8_t cursor_start, uint8_t cursor_end) {
	outb(VGA_CTRL_PORT, 0x0A);
	outb(VGA_DATA_PORT, (inb(VGA_DATA_PORT) & 0xC0) | cursor_start);
	outb(VGA_CTRL_PORT, 0x0B);
	outb(VGA_DATA_PORT, (inb(VGA_DATA_PORT) & 0xE0) | cursor_end);
}

/**
 * @brief Updates the cursor position on the screen.
 * @param x The column coordinate.
 * @param y The row coordinate.
 */
void update_cursor(int x, int y) {
	uint16_t pos = y * VGA_WIDTH + x;
	outb(VGA_CTRL_PORT, 0x0F);
	outb(VGA_DATA_PORT, (uint8_t) (pos & 0xFF));
	outb(VGA_CTRL_PORT, 0x0E);
	outb(VGA_DATA_PORT, (uint8_t) ((pos >> 8) & 0xFF));
}

/**
 * @brief Initializes the terminal, clearing the screen and setting the initial color.
 */
void terminal_initialize(void) {
	terminal_row = 0;
	terminal_column = 0;
	terminal_color = vga_entry_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
	terminal_buffer = (uint16_t*) VGA_MEMORY_ADDR;
	for (size_t y = 0; y < VGA_HEIGHT; y++) {
		for (size_t x = 0; x < VGA_WIDTH; x++) {
			terminal_buffer[y * VGA_WIDTH + x] = vga_entry(' ', terminal_color);
		}
	}
    enable_cursor(14, 15);
    update_cursor(0, 0);
}

/**
 * @brief Sets the current terminal color for future output.
 * @param color The VGA color attribute.
 */
void terminal_setcolor(uint8_t color) {
    terminal_color = color;
}

/**
 * @brief Places a character at a specific location on the screen.
 * @param c The character to display.
 * @param color The color attribute.
 * @param x The column coordinate.
 * @param y The row coordinate.
 */
void terminal_putentryat(char c, uint8_t color, size_t x, size_t y) {
	terminal_buffer[y * VGA_WIDTH + x] = vga_entry(c, color);
}

/**
 * @brief Scrolls the terminal screen up by one line.
 */
void terminal_scroll(void) {
    for (size_t y = 0; y < VGA_HEIGHT - 1; y++) {
        for (size_t x = 0; x < VGA_WIDTH; x++) {
            terminal_buffer[y * VGA_WIDTH + x] = terminal_buffer[(y + 1) * VGA_WIDTH + x];
        }
    }
    for (size_t x = 0; x < VGA_WIDTH; x++) {
        terminal_putentryat(' ', terminal_color, x, VGA_HEIGHT - 1);
    }
    terminal_row = VGA_HEIGHT - 1;
}

/**
 * @brief Outputs a single character to the terminal, handling special characters like newline.
 * @param c The character to output.
 */
void terminal_putchar(char c) {
    if (c == '\n') {
        terminal_column = 0;
        terminal_row++;
        if (terminal_row == VGA_HEIGHT) terminal_scroll();
        update_cursor(terminal_column, terminal_row);
        return;
    } else if (c == '\b') {
        if (terminal_column > 0) {
            terminal_column--;
            terminal_putentryat(' ', terminal_color, terminal_column, terminal_row);
        }
        update_cursor(terminal_column, terminal_row);
        return;
    }
	terminal_putentryat(c, terminal_color, terminal_column, terminal_row);
	if (++terminal_column == VGA_WIDTH) {
		terminal_column = 0;
		if (++terminal_row == VGA_HEIGHT) terminal_scroll();
	}
    update_cursor(terminal_column, terminal_row);
}

/**
 * @brief Writes a buffer of characters to the terminal.
 * @param data Pointer to the character buffer.
 * @param size The number of characters to write.
 */
void terminal_write(const char* data, size_t size) {
	for (size_t i = 0; i < size; i++) terminal_putchar(data[i]);
}

/**
 * @brief Writes a null-terminated string to the terminal.
 * @param data Pointer to the null-terminated string.
 */
void terminal_writestring(const char* data) {
    if (!data) return;
    terminal_write(data, strlen(data));
}
