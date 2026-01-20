#include "editor.h"
#include "fs.h"
#include "shell.h"
#include "kheap.h"
#include <stddef.h>

extern void terminal_initialize();
extern void terminal_writestring(const char* data);
extern void terminal_putchar(char c);
extern void terminal_setcolor(uint8_t color);
extern void terminal_putentryat(char c, uint8_t color, size_t x, size_t y);
extern void update_cursor(int x, int y);
extern void shell_init(); // To return to shell

#define EDITOR_WIDTH 80
#define EDITOR_HEIGHT 25
#define MAX_FILE_SIZE 4096

char* edit_buffer;
int edit_cursor_x = 0;
int edit_cursor_y = 0;
int edit_len = 0;
char current_filename[32];
int editor_running = 0;

void draw_status_bar() {
    for(int x=0; x<EDITOR_WIDTH; x++) terminal_putentryat(' ', 0x70, x, EDITOR_HEIGHT-1);
    
    char* title = " JexOS Editor v0.1 ";
    for(int i=0; title[i]; i++) terminal_putentryat(title[i], 0x70, i, EDITOR_HEIGHT-1);

    char* help = " Ctrl+S: Save | Ctrl+Q: Quit ";
    int help_len = 0; while(help[help_len]) help_len++;
    for(int i=0; help[i]; i++) terminal_putentryat(help[i], 0x70, EDITOR_WIDTH - help_len + i, EDITOR_HEIGHT-1);
}

void render_text() {
    terminal_initialize(); // Clear screen
    draw_status_bar();
    
    int x = 0, y = 0;
    for (int i = 0; i < edit_len; i++) {
        char c = edit_buffer[i];
        if (c == '\n') {
            x = 0;
            y++;
        } else {
            terminal_putentryat(c, 0x07, x, y);
            x++;
            if (x >= EDITOR_WIDTH) { x = 0; y++; }
        }
    }
    update_cursor(edit_cursor_x, edit_cursor_y);
}

void save_file() {
    // 1. Check if file exists, if not touch it
    // For now we assume touch was done or fs_open handles it?
    // Our fs_open is simple. Let's just try to open.
    
    int fd = fs_open(current_filename, 0);
    if (fd == -1) {
        // Try creating it? Our fs doesn't have create yet.
        // Assume user ran 'touch' first or use fat12_touch hack
        // But we are in kernel mode so we can cheat.
        // Let's just assume it exists for this V1.
    }
    
    if (fd != -1) {
        fs_write(fd, edit_buffer, edit_len);
        fs_close(fd);
        
        // Flash status bar
        for(int x=0; x<EDITOR_WIDTH; x++) terminal_putentryat(' ', 0x20, x, EDITOR_HEIGHT-1);
        char* msg = " File Saved! ";
        for(int i=0; msg[i]; i++) terminal_putentryat(msg[i], 0x20, i, EDITOR_HEIGHT-1);
    }
}

void editor_input(char key) {
    if (!editor_running) return;

    if (key == 0x11) { // Ctrl+Q
        editor_running = 0;
        terminal_initialize();
        shell_init();
        return;
    }
    else if (key == 0x13) { // Ctrl+S
        save_file();
        return;
    }
    // Arrows (0x80 - 0x83)
    else if ((unsigned char)key == 0x80) { if (edit_cursor_y > 0) edit_cursor_y--; } // Up
    else if ((unsigned char)key == 0x81) { edit_cursor_y++; } // Down
    else if ((unsigned char)key == 0x82) { if (edit_cursor_x > 0) edit_cursor_x--; } // Left
    else if ((unsigned char)key == 0x83) { if (edit_cursor_x < EDITOR_WIDTH-1) edit_cursor_x++; } // Right
    else if (key == '\b') {
        // Simple backspace (only at end of buffer for now to keep it simple)
        if (edit_len > 0) {
            edit_len--;
            edit_buffer[edit_len] = 0;
            // Need to update cursor logic to follow deletion
            if (edit_cursor_x > 0) edit_cursor_x--;
        }
    }
    else {
        // Insert char
        if (edit_len < MAX_FILE_SIZE - 1) {
            edit_buffer[edit_len++] = key;
            edit_buffer[edit_len] = 0;
            if (key == '\n') {
                edit_cursor_x = 0;
                edit_cursor_y++;
            } else {
                edit_cursor_x++;
                if (edit_cursor_x >= EDITOR_WIDTH) {
                    edit_cursor_x = 0;
                    edit_cursor_y++;
                }
            }
        }
    }
    render_text();
}

void start_editor(const char* filename) {
    // Copy filename
    for(int i=0; i<31 && filename[i]; i++) current_filename[i] = filename[i];
    current_filename[31] = 0;

    // Alloc buffer
    if (!edit_buffer) edit_buffer = (char*)kmalloc(MAX_FILE_SIZE);
    
    // Read existing file
    int fd = fs_open(filename, 0);
    if (fd != -1) {
        edit_len = fs_read(fd, edit_buffer, MAX_FILE_SIZE);
        fs_close(fd);
    } else {
        edit_len = 0;
    }
    edit_buffer[edit_len] = 0;

    editor_running = 1;
    edit_cursor_x = 0;
    edit_cursor_y = 0;
    
    render_text();
}
