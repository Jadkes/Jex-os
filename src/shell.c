#include "shell.h"
#include "keyboard.h"
#include "rtc.h"
#include "pmm.h"
#include "fs.h"
#include "elf.h"
#include "tcc.h"
#include "exec.h"
#include "syscall.h"
#include "jexfs.h"
#include <stddef.h>
#include <stdint.h>

// Forward declarations for string functions
char* strchr(const char* s, int c);
char* strncpy(char* dest, const char* src, size_t n);
char* strcpy(char* dest, const char* src);
size_t strlen(const char* s);

extern void terminal_writestring(const char* data);
extern void terminal_putchar(char c);
extern void terminal_initialize();
extern void terminal_setcolor(uint8_t color);
extern void jump_to_user_mode(uint32_t entry, uint32_t stack);
extern void default_user_start();
extern void log_serial(const char* str);
extern void log_hex_serial(uint32_t n);
extern void update_cursor(int x, int y);
extern void terminal_putentryat(char c, uint8_t color, size_t x, size_t y);
extern size_t terminal_row; 

extern void editor_input(char key);
extern int editor_running;

#include "kheap.h"
#include "pmm.h"

// Missing function declarations
extern void beep(int freq, int duration);
extern void start_editor(const char* filename);
extern void sleep(int seconds);
extern void reboot(void);
extern void shutdown(void);
extern void set_kernel_stack(uint32_t stack);

#define SHELL_BUFFER_SIZE 256
#define MAX_HISTORY 5

char shell_buffer[SHELL_BUFFER_SIZE];
char history[MAX_HISTORY][SHELL_BUFFER_SIZE];
int history_count = 0;
int history_index = 0;

int buffer_len = 0;
int cursor_pos = 0;

void play_tune() {
    beep(392, 100); beep(523, 100); beep(659, 100);
    beep(784, 300); beep(659, 150); beep(784, 400);
}

void int_to_string(int n, char* str) {
    int i = 0; int is_neg = 0;
    if (n == 0) { str[0] = '0'; str[1] = '\0'; return; }
    if (n < 0) { is_neg = 1; n = -n; }
    while (n != 0) { str[i++] = (n % 10) + '0'; n /= 10; }
    if (is_neg) str[i++] = '-';
    str[i] = '\0';
    int start = 0; int end = i - 1;
    while (start < end) {
        char temp = str[start]; str[start] = str[end]; str[end] = temp;
        start++; end--;
    }
}

int atoi(const char* str) {
    int res = 0;
    for (int i = 0; str[i] != '\0'; ++i) {
        if (str[i] >= '0' && str[i] <= '9') res = res * 10 + str[i] - '0';
    }
    return res;
}

int strcmp(const char* s1, const char* s2) {
    while(*s1 && (*s1 == *s2)) { s1++; s2++; }
    return *(const unsigned char*)s1 - *(const unsigned char*)s2;
}

int strncmp(const char* s1, const char* s2, size_t n) {
    while(n--) {
        if(*s1 != *s2) return *(const unsigned char*)s1 - *(const unsigned char*)s2;
        if(*s1 == 0) break;
        s1++; s2++;
    }
    return 0;
}

void print_prompt() {
    terminal_setcolor(0x02);
    terminal_writestring("root@jexos:/> ");
    terminal_setcolor(0x07);
}

void shell_refresh_line() {
    for (int i = 14; i < 80; i++) {
        terminal_putentryat(' ', 0x07, i, terminal_row);
    }
    for (int i = 0; i < buffer_len; i++) {
        terminal_putentryat(shell_buffer[i], 0x07, 14 + i, terminal_row);
    }
    update_cursor(14 + cursor_pos, terminal_row);
}

void print_logo() {
    terminal_setcolor(0x0B); 
    terminal_writestring("      _             ___  ____  \n");
    terminal_writestring("     | | _____  __ / _ \/ ___| \n");
    terminal_writestring("  _  | |/ _ \ \/ /| | | \___ \ \n");
    terminal_writestring(" | |_| |  __/>  < | |_| |___) |\n");
    terminal_writestring("  \___/ \___/_/\_\ \___/|____/ \n");
    terminal_setcolor(0x07);
}

void help_command() {
    terminal_writestring("Available commands:\n");
    terminal_writestring("  help      - Show this help message\n");
    terminal_writestring("  ls        - List files (JexFS)\n");
    terminal_writestring("  touch <f> - Create file\n");
    terminal_writestring("  vix <f>   - Open Text Editor\n");
    terminal_writestring("  cat <f>   - Read file\n");
    terminal_writestring("  mkcode    - Create persistent hello.c\n");
    terminal_writestring("  tcc <f>   - Compile and run C file\n");
    terminal_writestring("  cc <f>    - Compile C file to ELF\n");
    terminal_writestring("  ./<f>     - Execute file\n");
    terminal_writestring("  free      - Show memory usage\n");
    terminal_writestring("  reboot    - Restart JexOS\n");
    terminal_writestring("  shutdown  - Power off JexOS\n");
}

void execute_command() {
    terminal_writestring("\n"); 
    
    if (buffer_len > 0) {
        if (history_count < MAX_HISTORY) {
            for(int i=0; i<=buffer_len; i++) history[history_count][i] = shell_buffer[i];
            history_count++;
        } else {
            for (int i = 0; i < MAX_HISTORY - 1; i++) {
                for(int j=0; j<SHELL_BUFFER_SIZE; j++) history[i][j] = history[i+1][j];
            }
            for(int i=0; i<=buffer_len; i++) history[MAX_HISTORY-1][i] = shell_buffer[i];
        }
        history_index = history_count;
    }

    if (strcmp(shell_buffer, "help") == 0) help_command();
    else if (strcmp(shell_buffer, "clear") == 0) { terminal_initialize(); print_logo(); }
    else if (strcmp(shell_buffer, "ls") == 0) jexfs_list_dir();
    else if (strncmp(shell_buffer, "touch ", 6) == 0) fs_create(shell_buffer + 6);
    else if (strncmp(shell_buffer, "cat ", 4) == 0) {
        int fd = fs_open(shell_buffer + 4, 0);
        if (fd != -1) {
            char buf[1024];
            int bytes = fs_read(fd, buf, 1023);
            if (bytes > 0) {
                buf[bytes] = '\0';
                terminal_writestring(buf);
                terminal_writestring("\n");
            }
            fs_close(fd);
        } else {
            terminal_writestring("File not found.\n");
        }
    }
    else if (strncmp(shell_buffer, "vix ", 4) == 0) start_editor(shell_buffer + 4);
    else if (strcmp(shell_buffer, "music") == 0) play_tune();
    else if (strcmp(shell_buffer, "reboot") == 0) reboot();
    else if (strcmp(shell_buffer, "shutdown") == 0) shutdown();
    else if (strcmp(shell_buffer, "mkcode") == 0) {
        terminal_writestring("Creating hello.c on Persistent Disk...\n");
        const char* code = "int main() {\n  printf(\"Hello from JexFS Persistence!\\n\");\n  return 0;\n}";
        fs_create("hello.c");
        int fd = fs_open("hello.c", 0);
        fs_write(fd, code, strlen(code));
        fs_close(fd);
        terminal_writestring("Done. Use 'ls' to see it.\n");
    }
    else if (strncmp(shell_buffer, "tcc ", 4) == 0) {
        char* filename = shell_buffer + 4;
        while (*filename == ' ') filename++;
        int fd = fs_open(filename, 0);
        if (fd < 0) {
            terminal_writestring("Failed to open C source file\n");
        } else {
            char* source = (char*)kmalloc(4096);
            int bytes = fs_read(fd, source, 4095);
            source[bytes] = '\0';
            fs_close(fd);
            char* argv[] = {filename, NULL};
            extern int exec_c_code(const char* c_source, char** argv);
            exec_c_code(source, argv);
            kfree(source);
        }
    }
    else if (strncmp(shell_buffer, "cc ", 3) == 0) {
        char filename[64];
        char output_file[64] = "a.out";
        char* params = shell_buffer + 3;
        while (*params == ' ') params++;
        
        int i = 0;
        while (params[i] && params[i] != ' ' && i < 63) {
            filename[i] = params[i];
            i++;
        }
        filename[i] = '\0';

        // Check for -o flag
        char* next = params + i;
        while (*next == ' ') next++;
        if (strncmp(next, "-o", 2) == 0) {
            next += 2;
            while (*next == ' ') next++;
            int j = 0;
            while (next[j] && next[j] != ' ' && j < 63) {
                output_file[j] = next[j];
                j++;
            }
            output_file[j] = '\0';
        }

        int fd = fs_open(filename, 0);
        if (fd < 0) {
            terminal_writestring("Failed to open source file: ");
            terminal_writestring(filename);
            terminal_writestring("\n");
        } else {
            char* source = (char*)kmalloc(4096);
            int bytes = fs_read(fd, source, 4095);
            source[bytes] = '\0';
            fs_close(fd);
            tcc_state_t* tcc = tcc_new();
            if (tcc && tcc_compile_string(tcc, source) == 0) {
                uint8_t* elf_data; uint32_t elf_size;
                if (tcc_output_memory(tcc, &elf_data, &elf_size) == 0) {
                    // Create/Overwrite output file
                    int out_fd = fs_open(output_file, 0);
                    if (out_fd < 0) {
                        fs_create(output_file);
                        out_fd = fs_open(output_file, 0);
                    }
                    fs_write(out_fd, elf_data, elf_size);
                    fs_close(out_fd);
                    terminal_writestring("Compilation successful: ");
                    terminal_writestring(output_file);
                    terminal_writestring(" created\n");
                }
            } else { terminal_writestring("Compilation failed\n"); }
            if (tcc) tcc_delete(tcc);
            kfree(source);
        }
    }
    else if (strncmp(shell_buffer, "./", 2) == 0) {
        char* filename = shell_buffer + 2;
        char* argv[] = {filename, NULL};
        extern int execve_file(const char* filename, char** argv, char** envp);
        execve_file(filename, argv, NULL);
    }
    else if (strcmp(shell_buffer, "free") == 0) {
        char buf[32];
        terminal_writestring("Memory Status:\n  Total: "); int_to_string(pmm_get_total_memory() / 1024, buf); terminal_writestring(buf); terminal_writestring(" KB\n");
        terminal_writestring("  Used:  "); int_to_string(pmm_get_used_memory() / 1024, buf); terminal_writestring(buf); terminal_writestring(" KB\n");
        terminal_writestring("  Free:  "); int_to_string(pmm_get_free_memory() / 1024, buf); terminal_writestring(buf); terminal_writestring(" KB\n");
    }
    else if (shell_buffer[0] != '\0') {
        terminal_writestring("Unknown command: "); terminal_writestring(shell_buffer); terminal_writestring("\n");
    }

    for (int i = 0; i < SHELL_BUFFER_SIZE; i++) shell_buffer[i] = 0;
    buffer_len = 0;
    cursor_pos = 0;
    print_prompt();
}

void shell_init() {
    print_logo();
    terminal_writestring("\nWelcome to JexOS v0.3 Persistence Release!\nType 'help' for a list of commands.\n\n");
    print_prompt();
}

void shell_loop() {
    print_prompt();
    while(1) __asm__ volatile("hlt");
}

void shell_main() {
    shell_init();
    while(1) __asm__ volatile("hlt");
}

void shell_input(char key) {
    if (editor_running) { editor_input(key); return; }
    if (key == '\n') { execute_command(); return; }
    if ((unsigned char)key == 0x82) { if (cursor_pos > 0) cursor_pos--; shell_refresh_line(); return; }
    if ((unsigned char)key == 0x83) { if (cursor_pos < buffer_len) cursor_pos++; shell_refresh_line(); return; }
    if ((unsigned char)key == 0x80) {
        if (history_count > 0) {
            if (history_index > 0) history_index--;
            int i = 0;
            while(history[history_index][i] && i < SHELL_BUFFER_SIZE-1) { shell_buffer[i] = history[history_index][i]; i++; }
            shell_buffer[i] = 0; buffer_len = i; cursor_pos = i; shell_refresh_line();
        }
        return;
    }
    if ((unsigned char)key == 0x81) {
        if (history_count > 0 && history_index < history_count) {
            history_index++;
            if (history_index == history_count) { shell_buffer[0] = 0; buffer_len = 0; cursor_pos = 0; }
            else { int i = 0; while(history[history_index][i] && i < SHELL_BUFFER_SIZE-1) { shell_buffer[i] = history[history_index][i]; i++; }
                shell_buffer[i] = 0; buffer_len = i; cursor_pos = i; }
            shell_refresh_line();
        }
        return;
    }
    if (key == '\b') {
        if (cursor_pos > 0) {
            for (int i = cursor_pos - 1; i < buffer_len; i++) shell_buffer[i] = shell_buffer[i+1];
            buffer_len--; cursor_pos--; shell_refresh_line();
        }
    } else if (buffer_len < SHELL_BUFFER_SIZE - 1) {
        if (cursor_pos < buffer_len) { for (int i = buffer_len; i > cursor_pos; i--) shell_buffer[i] = shell_buffer[i-1]; }
        shell_buffer[cursor_pos] = key; buffer_len++; cursor_pos++; shell_buffer[buffer_len] = 0; shell_refresh_line();
    }
}
