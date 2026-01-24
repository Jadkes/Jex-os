#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "gdt.h"
#include "idt.h"
#include "isr.h"
#include "irq.h"
#include "keyboard.h"
#include "shell.h"
#include "multiboot.h"
#include "pmm.h"
#include "paging.h"
#include "ports.h"
#include "kheap.h"
#include "fat12.h"
#include "timer.h"
#include "fs.h"
#include "task.h"

// Kernel stack for user mode transitions
uint32_t kernel_stack_top;

#define PORT 0x3f8
void init_serial() {
   outb(PORT + 1, 0x00);
   outb(PORT + 3, 0x80);
   outb(PORT + 0, 0x03);
   outb(PORT + 1, 0x00);
   outb(PORT + 3, 0x03);
   outb(PORT + 2, 0xC7);
   outb(PORT + 4, 0x0B);
}
int is_transmit_empty() { return inb(PORT + 5) & 0x20; }
void write_serial(char a) {
   while (is_transmit_empty() == 0);
   outb(PORT, a);
}

int is_serial_received() {
   return inb(PORT + 5) & 1;
}

char read_serial() {
   while (is_serial_received() == 0);
   return inb(PORT);
}

void log_serial(const char* str) {
    for (int i = 0; str[i] != '\0'; i++) write_serial(str[i]);
}
void log_hex_serial(uint32_t n) {
    const char *digits = "0123456789ABCDEF";
    log_serial("0x");
    for (int i = 28; i >= 0; i -= 4) write_serial(digits[(n >> i) & 0xF]);
    log_serial("\n");
}

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

static inline uint8_t vga_entry_color(enum vga_color fg, enum vga_color bg) { return fg | bg << 4; }
static inline uint16_t vga_entry(unsigned char uc, uint8_t color) { return (uint16_t) uc | (uint16_t) color << 8; }

size_t strlen(const char* str) {
	size_t len = 0;
	while (str[len]) len++;
	return len;
}

static const size_t VGA_WIDTH = 80;
static const size_t VGA_HEIGHT = 25;
size_t terminal_row;
size_t terminal_column;
uint8_t terminal_color;
uint16_t* terminal_buffer;

void enable_cursor(uint8_t cursor_start, uint8_t cursor_end) {
	outb(0x3D4, 0x0A);
	outb(0x3D5, (inb(0x3D5) & 0xC0) | cursor_start);
	outb(0x3D4, 0x0B);
	outb(0x3D5, (inb(0x3D5) & 0xE0) | cursor_end);
}

void update_cursor(int x, int y) {
	uint16_t pos = y * VGA_WIDTH + x;
	outb(0x3D4, 0x0F);
	outb(0x3D5, (uint8_t) (pos & 0xFF));
	outb(0x3D4, 0x0E);
	outb(0x3D5, (uint8_t) ((pos >> 8) & 0xFF));
}

void terminal_initialize(void) {
	terminal_row = 0;
	terminal_column = 0;
	terminal_color = vga_entry_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
	terminal_buffer = (uint16_t*) 0xB8000;
	for (size_t y = 0; y < VGA_HEIGHT; y++) {
		for (size_t x = 0; x < VGA_WIDTH; x++) {
			terminal_buffer[y * VGA_WIDTH + x] = vga_entry(' ', terminal_color);
		}
	}
    enable_cursor(14, 15);
    update_cursor(0, 0);
}

void terminal_setcolor(uint8_t color) { terminal_color = color; }

void terminal_putentryat(char c, uint8_t color, size_t x, size_t y) {
	terminal_buffer[y * VGA_WIDTH + x] = vga_entry(c, color);
}

void terminal_scroll(void) {
    for (size_t y = 0; y < VGA_HEIGHT - 1; y++) {
        for (size_t x = 0; x < VGA_WIDTH; x++) {
            terminal_buffer[y * VGA_WIDTH + x] = terminal_buffer[(y + 1) * VGA_WIDTH + x];
        }
    }
    for (size_t x = 0; x < VGA_WIDTH; x++) terminal_putentryat(' ', terminal_color, x, VGA_HEIGHT - 1);
    terminal_row = VGA_HEIGHT - 1;
}

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

void terminal_write(const char* data, size_t size) {
	for (size_t i = 0; i < size; i++) terminal_putchar(data[i]);
}

void terminal_writestring(const char* data) { terminal_write(data, strlen(data)); }

void kernel_main(uint32_t magic, multiboot_info_t* mboot_info) {
	terminal_initialize();
    init_serial();
    log_serial("JexOS Kernel Started\n");
    init_gdt();
    init_idt();
    isr_install();
    init_irq();
    init_keyboard();
    if (magic == 0x2BADB002) pmm_init(mboot_info);
    init_paging();
    init_kheap(0x1000000);
    init_fat12();
    fs_init();
    init_tasking();
    init_timer(100);
    
    // Initialize kernel stack top for user mode transitions
    extern void* kmalloc(size_t size);
    void* kernel_stack = kmalloc(8192);
    kernel_stack_top = (uint32_t)kernel_stack + 8192;
    
    // Initialize syscalls
    extern void init_syscalls();
    init_syscalls();
    
    __asm__ volatile("sti");
    
    shell_main();
    
    while(1) __asm__ volatile("hlt");
}
