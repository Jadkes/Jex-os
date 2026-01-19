#include "shell.h"
#include "rtc.h"
#include "pmm.h"
#include "fat12.h"
#include "power.h"
#include <stddef.h>
#include <stdint.h>

/* Helper functions from other files */
extern void terminal_writestring(const char* data);
extern void terminal_putchar(char c);
extern void terminal_initialize();
extern void terminal_setcolor(uint8_t color);
extern void jump_to_user_mode();

/* Simple integer to string conversion */
void int_to_string(int n, char* str) {
    int i = 0;
    int is_neg = 0;
    if (n == 0) {
        str[0] = '0';
        str[1] = '\0';
        return;
    }
    if (n < 0) {
        is_neg = 1;
        n = -n;
    }
    while (n != 0) {
        str[i++] = (n % 10) + '0';
        n /= 10;
    }
    if (is_neg) str[i++] = '-';
    str[i] = '\0';

    // Reverse the string
    int start = 0;
    int end = i - 1;
    while (start < end) {
        char temp = str[start];
        str[start] = str[end];
        str[end] = temp;
        start++;
        end--;
    }
}

int strcmp(const char* s1, const char* s2)
{
    while(*s1 && (*s1 == *s2))
    {
        s1++;
        s2++;
    }
    return *(const unsigned char*)s1 - *(const unsigned char*)s2;
}

int strncmp(const char* s1, const char* s2, size_t n)
{
    while(n--)
    {
        if(*s1 != *s2)
            return *(const unsigned char*)s1 - *(const unsigned char*)s2;
        if(*s1 == 0)
            break;
        s1++;
        s2++;
    }
    return 0;
}

#define SHELL_BUFFER_SIZE 256
char shell_buffer[SHELL_BUFFER_SIZE];
int buffer_index = 0;

void print_prompt()
{
    terminal_setcolor(0x02); // Green
    terminal_writestring("root@jexos:/> ");
    terminal_setcolor(0x07); // Light Grey
}

void print_logo()
{
    terminal_setcolor(0x03); // Cyan
    terminal_writestring("       __          ____  _____    \n");
    terminal_writestring("      / /__  _  __/ __ \\/ ___/   \n");
    terminal_writestring(" __  / / _ \\| |/_/ / / /\\__ \    \n");
    terminal_writestring("/ /_/ /  __/>  </ /_/ /___/ /     \n");
    terminal_writestring("\\____/\___/_/|_|\\____//____/      \n");
    terminal_setcolor(0x07);
}

void help_command()
{
    terminal_writestring("Available commands:\n");
    terminal_writestring("  help      - Show this help message\n");
    terminal_writestring("  clear     - Clear the screen\n");
    terminal_writestring("  logo      - Show the JexOS logo\n");
    terminal_writestring("  ls        - List files\n");
    terminal_writestring("  touch <f> - Create file\n");
    terminal_writestring("  echo <f><t>- Write to file\n");
    terminal_writestring("  cat <f>   - Read file\n");
    terminal_writestring("  rm <f>    - Delete file\n");
    terminal_writestring("  time      - Show system time\n");
    terminal_writestring("  date      - Show system date\n");
    terminal_writestring("  free      - Memory status\n");
    terminal_writestring("  reboot    - Restart JexOS\n");
    terminal_writestring("  shutdown  - Power off JexOS\n");
    terminal_writestring("  usermode  - Enter Ring 3 (RESTRICTED)\n");
    terminal_writestring("  panic     - Trigger a kernel panic\n");
}

void execute_command()
{
    terminal_writestring("\n"); 

    if (strcmp(shell_buffer, "help") == 0) {
        help_command();
    }
    else if (strcmp(shell_buffer, "clear") == 0) {
        terminal_initialize();
        print_logo();
    }
    else if (strcmp(shell_buffer, "logo") == 0) {
        print_logo();
    }
    else if (strcmp(shell_buffer, "ls") == 0) {
        fat12_ls();
    }
    else if (strncmp(shell_buffer, "touch ", 6) == 0) {
        fat12_touch(shell_buffer + 6);
    }
    else if (strncmp(shell_buffer, "cat ", 4) == 0) {
        fat12_cat(shell_buffer + 4);
    }
    else if (strncmp(shell_buffer, "rm ", 3) == 0) {
        fat12_rm(shell_buffer + 3);
    }
    else if (strncmp(shell_buffer, "echo ", 5) == 0) {
        char* filename = shell_buffer + 5;
        char* text = NULL;
        for (int i = 0; filename[i] != '\0'; i++) {
            if (filename[i] == ' ') {
                filename[i] = '\0';
                text = filename + i + 1;
                break;
            }
        }
        if (text) fat12_echo(filename, text);
        else terminal_writestring("Usage: echo <filename> <text>\n");
    }
    else if (strcmp(shell_buffer, "reboot") == 0) {
        reboot();
    }
    else if (strcmp(shell_buffer, "shutdown") == 0) {
        shutdown();
    }
    else if (strcmp(shell_buffer, "usermode") == 0) {
        terminal_writestring("Dropping privileges to Ring 3...\n");
        jump_to_user_mode();
    }
    else if (strcmp(shell_buffer, "time") == 0) {
        rtc_time_t t = read_rtc();
        char buf[10];
        int_to_string(t.hours, buf);
        if (t.hours < 10) terminal_putchar('0');
        terminal_writestring(buf);
        terminal_putchar(':');
        int_to_string(t.minutes, buf);
        if (t.minutes < 10) terminal_putchar('0');
        terminal_writestring(buf);
        terminal_putchar(':');
        int_to_string(t.seconds, buf);
        if (t.seconds < 10) terminal_putchar('0');
        terminal_writestring(buf);
        terminal_writestring(" UTC\n");
    }
    else if (strcmp(shell_buffer, "date") == 0) {
        rtc_time_t t = read_rtc();
        char buf[10];
        int_to_string(t.day, buf);
        if (t.day < 10) terminal_putchar('0');
        terminal_writestring(buf);
        terminal_putchar('/');
        int_to_string(t.month, buf);
        if (t.month < 10) terminal_putchar('0');
        terminal_writestring(buf);
        terminal_putchar('/');
        int_to_string(t.year, buf);
        terminal_writestring(buf);
        terminal_writestring("\n");
    }
    else if (strcmp(shell_buffer, "free") == 0) {
        uint32_t free_mem = pmm_get_free_memory();
        uint32_t used_mem = pmm_get_used_memory();
        uint32_t total_mem = pmm_get_total_memory();
        char buf[32];
        terminal_writestring("Memory Status:\n");
        terminal_writestring("  Total: "); int_to_string(total_mem / 1024, buf); terminal_writestring(buf); terminal_writestring(" KB\n");
        terminal_writestring("  Used:  "); int_to_string(used_mem / 1024, buf); terminal_writestring(buf); terminal_writestring(" KB\n");
        terminal_writestring("  Free:  "); int_to_string(free_mem / 1024, buf); terminal_writestring(buf); terminal_writestring(" KB\n");
    }
    else if (strcmp(shell_buffer, "panic") == 0) {
        int a = 1, b = 0;
        int c = a / b;
        terminal_putchar(c);
    }
    else if (shell_buffer[0] == '\0') {}
    else {
        terminal_writestring("Unknown command: ");
        terminal_writestring(shell_buffer);
        terminal_writestring("\n");
    }

    for (int i = 0; i < SHELL_BUFFER_SIZE; i++) shell_buffer[i] = 0;
    buffer_index = 0;
    print_prompt();
}

void shell_init()
{
    print_logo();
    terminal_writestring("\nWelcome to JexOS v0.1!\n");
    terminal_writestring("Type 'help' for a list of commands.\n\n");
    print_prompt();
    for (int i = 0; i < SHELL_BUFFER_SIZE; i++) shell_buffer[i] = 0;
    buffer_index = 0;
}

void shell_input(char key)
{
    if (key == '\n')
    {
        execute_command();
    }
    else if (key == '\b')
    {
        if (buffer_index > 0)
        {
            buffer_index--;
            shell_buffer[buffer_index] = 0;
            terminal_putchar('\b');
        }
    }
    else
    {
        if (buffer_index < SHELL_BUFFER_SIZE - 1)
        {
            shell_buffer[buffer_index] = key;
            buffer_index++;
            shell_buffer[buffer_index] = 0;
            terminal_putchar(key);
        }
    }
}
