#include "editor.h"
#include "fs.h"
#include "shell.h"
#include "kheap.h"
#include "string.h"
#include "tcc.h"
#include "exec.h"
#include <stddef.h>

extern void terminal_initialize();
extern void terminal_writestring(const char* data);
extern void terminal_putchar(char c);
extern void terminal_setcolor(uint8_t color);
extern void terminal_putentryat(char c, uint8_t color, size_t x, size_t y);
extern void update_cursor(int x, int y);
extern void shell_init();
extern void int_to_string(int n, char* str);

#define EDITOR_WIDTH 80
#define EDITOR_HEIGHT 25
#define MAX_FILE_SIZE 8192
#define VISIBLE_HEIGHT (EDITOR_HEIGHT - 1)

char* edit_buffer;
int edit_cursor_x = 0;
int edit_cursor_y = 0;
int edit_scroll_y = 0;
int edit_len = 0;
char current_filename[32];
int editor_running = 0;

int save_status = 0;
int quit_confirm = 0;
int is_dirty = 0;

static const char* keywords[] = {"int", "void", "return", "if", "else", "for", "while", "char", "struct", "static", "extern", NULL};
static const char* syscalls[] = {"print", "printf", "malloc", "free", "open", "read", "write", "close", NULL};

int is_alpha_num(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

int is_digit(char c) {
    return (c >= '0' && c <= '9');
}

uint8_t get_char_color(int pos) {
    // Check for comments
    int line_start = pos;
    while (line_start > 0 && edit_buffer[line_start-1] != '\n') {
        if (edit_buffer[line_start-1] == '/' && edit_buffer[line_start] == '/') return 0x08;
        line_start--;
    }

    // Strings
    int in_string = 0;
    for (int i = 0; i <= pos; i++) {
        if (edit_buffer[i] == '"' && (i == 0 || edit_buffer[i-1] != '\\')) {
            if (i < pos) in_string = !in_string;
        }
    }
    if (in_string || edit_buffer[pos] == '"') return 0x06;

    // Numbers
    if (is_digit(edit_buffer[pos]) && (pos == 0 || !is_alpha_num(edit_buffer[pos-1]))) return 0x0E;

    // Symbols
    char cc = edit_buffer[pos];
    if (cc == '(' || cc == ')' || cc == '{' || cc == '}' || cc == '[' || cc == ']') return 0x06;

    if (!is_alpha_num(cc)) return 0x07;
    
    if (pos == 0 || !is_alpha_num(edit_buffer[pos-1])) {
        char word[32];
        int len = 0;
        while (pos + len < edit_len && is_alpha_num(edit_buffer[pos + len]) && len < 31) {
            word[len] = edit_buffer[pos + len];
            len++;
        }
        word[len] = '\0';
        
        for (int i = 0; syscalls[i]; i++) { if (strcmp(word, syscalls[i]) == 0) return 0x0A; }

        int check_pos = pos + len;
        while (check_pos < edit_len && edit_buffer[check_pos] == ' ') check_pos++;
        if (check_pos < edit_len && edit_buffer[check_pos] == '(') return 0x0E;

        for (int i = 0; keywords[i]; i++) { if (strcmp(word, keywords[i]) == 0) return 0x0B; }
    } else {
        int start = pos;
        while (start > 0 && is_alpha_num(edit_buffer[start-1])) start--;
        return get_char_color(start);
    }
    return 0x07;
}

void draw_status_bar() {
    for(int x=0; x<EDITOR_WIDTH; x++) terminal_putentryat(' ', 0x70, x, EDITOR_HEIGHT-1);
    int pos = 0;
    char* prefix = " FILE: ";
    for(int i=0; prefix[i]; i++) terminal_putentryat(prefix[i], 0x70, pos++, EDITOR_HEIGHT-1);
    for(int i=0; current_filename[i]; i++) terminal_putentryat(current_filename[i], 0x70, pos++, EDITOR_HEIGHT-1);
    char l_str[10], c_str[10];
    int_to_string(edit_cursor_y + 1, l_str);
    int_to_string(edit_cursor_x + 1, c_str);
    char* mid = " | L: ";
    for(int i=0; mid[i]; i++) terminal_putentryat(mid[i], 0x70, pos++, EDITOR_HEIGHT-1);
    for(int i=0; l_str[i]; i++) terminal_putentryat(l_str[i], 0x70, pos++, EDITOR_HEIGHT-1);
    char* c_mid = " C: ";
    for(int i=0; c_mid[i]; i++) terminal_putentryat(c_mid[i], 0x70, pos++, EDITOR_HEIGHT-1);
    for(int i=0; c_str[i]; i++) terminal_putentryat(c_str[i], 0x70, pos++, EDITOR_HEIGHT-1);
    
    if (is_dirty) {
        char* d = " [MODIFIED]";
        for(int i=0; d[i]; i++) terminal_putentryat(d[i], 0x74, pos++, EDITOR_HEIGHT-1);
    }

    if (save_status == 1) {
        char* msg = " [SAVE OK] ";
        for(int i=0; msg[i]; i++) terminal_putentryat(msg[i], 0x2F, pos++, EDITOR_HEIGHT-1);
    } else if (save_status == 2) {
        char* msg = " [SAVE FAIL] ";
        for(int i=0; msg[i]; i++) terminal_putentryat(msg[i], 0x4F, pos++, EDITOR_HEIGHT-1);
    }
    char* help = " | ^S:Save ^B:Build ^Q:Quit ";
    int help_len = strlen(help);
    for(int i=0; help[i]; i++) terminal_putentryat(help[i], 0x70, EDITOR_WIDTH - help_len + i, EDITOR_HEIGHT-1);
}

void render_text() {
    terminal_initialize(); 
    draw_status_bar();
    if (quit_confirm) {
        int box_w = 40, box_h = 5;
        int bx = (EDITOR_WIDTH - box_w) / 2;
        int by = (EDITOR_HEIGHT - box_h) / 2;
        for (int y = by; y < by + box_h; y++) {
            for (int x = bx; x < bx + box_w; x++) terminal_putentryat(' ', 0x1F, x, y);
        }
        char* msg = "Save modified buffer? (y/n)";
        int msg_len = strlen(msg);
        for(int i=0; msg[i]; i++) terminal_putentryat(msg[i], 0x1F, bx + (box_w - msg_len)/2 + i, by + 2);
        update_cursor(bx + (box_w - msg_len)/2 + msg_len + 1, by + 2);
        return;
    }

    int x_offset = 4;
    int x = x_offset, y = 0;
    int line_num = 1;
    char ln_buf[5];

    for (int i = 0; i < edit_len; i++) {
        if (line_num > edit_scroll_y && y < VISIBLE_HEIGHT) {
            if (x == x_offset) {
                int_to_string(line_num, ln_buf);
                for(int k=0; k<3; k++) terminal_putentryat(' ', 0x03, k, y);
                for(int k=0; ln_buf[k]; k++) terminal_putentryat(ln_buf[k], 0x03, k, y);
                terminal_putentryat('|', 0x03, 3, y);
            }
            char c = edit_buffer[i];
            if (c != '\n') {
                uint8_t color = get_char_color(i);
                terminal_putentryat(c, color, x, y);
                x++;
                if (x >= EDITOR_WIDTH) { x = x_offset; y++; }
            }
        }
        if (edit_buffer[i] == '\n') {
            if (line_num > edit_scroll_y) {
                if (i > 0 && edit_buffer[i-1] != ';' && edit_buffer[i-1] != '{' && edit_buffer[i-1] != '}' && edit_buffer[i-1] != '\n') {
                    if (y < VISIBLE_HEIGHT) terminal_putentryat('/', 0x4F, x, y); 
                }
                y++;
            }
            x = x_offset;
            line_num++;
        }
        if (y >= VISIBLE_HEIGHT) break;
    }
    update_cursor(edit_cursor_x + x_offset, edit_cursor_y - edit_scroll_y);
}

int get_buffer_pos(int cursor_x, int cursor_y) {
    int x = 0, y = 0, pos = 0;
    while (pos < edit_len) {
        if (y == cursor_y && x == cursor_x) return pos;
        if (edit_buffer[pos] == '\n') { x = 0; y++; } 
        else { x++; if (x >= (EDITOR_WIDTH - 4)) { x = 0; y++; } }
        if (y > cursor_y) return pos;
        pos++;
    }
    return pos;
}

int save_file_internal() {
    int fd = fs_open(current_filename, 0);
    if (fd != -1) {
        int written = fs_write(fd, edit_buffer, edit_len);
        fs_close(fd);
        if (written == (int)edit_len) { save_status = 1; is_dirty = 0; return 0; }
    }
    save_status = 2; return -1;
}

void editor_input(char key) {
    if (!editor_running) return;
    if (quit_confirm) {
        if (key == 'y' || key == 'Y') { save_file_internal(); editor_running = 0; terminal_initialize(); shell_init(); }
        else if (key == 'n' || key == 'N') { editor_running = 0; terminal_initialize(); shell_init(); }
        quit_confirm = 0; if (editor_running) render_text(); return;
    }
    if (key == 0x11) { // Ctrl+Q
        if (is_dirty) { quit_confirm = 1; render_text(); } 
        else { editor_running = 0; terminal_initialize(); shell_init(); }
        return;
    }
    else if (key == 0x13) save_file_internal();
    else if (key == 0x02) {
        save_file_internal(); terminal_initialize();
        terminal_writestring("Compiling "); terminal_writestring(current_filename); terminal_writestring("\n");
        exec_c_code(edit_buffer, NULL);
        terminal_writestring("\nPress any key to return to editor."); return;
    }
    else if ((unsigned char)key == 0x80) { if (edit_cursor_y > 0) edit_cursor_y--; } 
    else if ((unsigned char)key == 0x81) { edit_cursor_y++; } 
    else if ((unsigned char)key == 0x82) { if (edit_cursor_x > 0) edit_cursor_x--; } 
    else if ((unsigned char)key == 0x83) { if (edit_cursor_x < (EDITOR_WIDTH-5)) edit_cursor_x++; } 
    else if (key == '\b') {
        int pos = get_buffer_pos(edit_cursor_x, edit_cursor_y);
        if (pos > 0) {
            for (int i = pos - 1; i < edit_len - 1; i++) edit_buffer[i] = edit_buffer[i+1];
            edit_len--; edit_buffer[edit_len] = 0; is_dirty = 1;
            if (edit_cursor_x > 0) edit_cursor_x--;
            else if (edit_cursor_y > 0) { 
                edit_cursor_y--; int line_pos = 0, ty = 0;
                while (ty < edit_cursor_y) { if(edit_buffer[line_pos++] == '\n') ty++; }
                int tx = 0; while(line_pos < edit_len && edit_buffer[line_pos] != '\n') { line_pos++; tx++; }
                edit_cursor_x = tx;
            }
        }
    }
    else {
        if (key < 32 && key != '\n' && key != '\t') return;
        if (edit_len < MAX_FILE_SIZE - 1) {
            int pos = get_buffer_pos(edit_cursor_x, edit_cursor_y);
            for (int i = edit_len; i > pos; i--) edit_buffer[i] = edit_buffer[i-1];
            edit_buffer[pos] = key;
            edit_len++;
            edit_buffer[edit_len] = 0;
            is_dirty = 1;
            if (key == '\n') { edit_cursor_x = 0; edit_cursor_y++; } 
            else { 
                edit_cursor_x++; 
                if (edit_cursor_x >= (EDITOR_WIDTH - 4)) { edit_cursor_x = 0; edit_cursor_y++; }
            }
        }
    }
    // Update scroll
    if (edit_cursor_y < edit_scroll_y) edit_scroll_y = edit_cursor_y;
    if (edit_cursor_y >= edit_scroll_y + VISIBLE_HEIGHT) edit_scroll_y = edit_cursor_y - VISIBLE_HEIGHT + 1;

    if (key != 0x13 && key != 0x02) save_status = 0;
    
    // Only render if serial port is empty (to speed up pasting)
    extern int is_serial_received();
    if (!is_serial_received()) render_text();
}

void start_editor(const char* filename) {
    for(int i=0; i<31 && filename[i]; i++) current_filename[i] = filename[i];
    current_filename[31] = 0;
    if (!edit_buffer) edit_buffer = (char*)kmalloc(MAX_FILE_SIZE);
    int fd = fs_open(filename, 0);
    if (fd != -1) { edit_len = fs_read(fd, edit_buffer, MAX_FILE_SIZE); fs_close(fd); } 
    else { edit_len = 0; }
    edit_buffer[edit_len] = 0; editor_running = 1; edit_cursor_x = 0; edit_cursor_y = 0; edit_scroll_y = 0;
    save_status = 0; quit_confirm = 0; is_dirty = 0;
    render_text();
}
