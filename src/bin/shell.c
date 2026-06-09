/**
 * @file shell.c
 * @brief Interactive Kernel Shell.
 *
 * Provides a command-line interface for the user to interact with the OS.
 * Supports file management, program execution, and basic system control.
 */

#include "shell.h"
#include "keyboard.h"
#include "serial.h"
#include "rtc.h"
#include "pmm.h"
#include "fs.h"
#include "elf.h"
#include "tcc.h"
#include "exec.h"
#include "syscall.h"
#include "jexfs.h"
#include "kheap.h"
#include "string.h"
#include "rtl8139.h"
#include "net.h"
#include "terminal.h"
#include "panic.h"
#include "klog.h"
#include "timer.h"
#include "task.h"
#include <stddef.h>
#include <stdint.h>

/* Forward declarations */
extern void jump_to_user_mode(uint32_t entry, uint32_t stack);
extern void editor_input(char key);
extern int editor_running;
extern void read_block(uint32_t block, uint8_t* buffer);
extern void beep(int freq, int duration);
extern void start_editor(const char* filename);
extern void sleep(uint32_t ms);
extern void reboot(void);
extern void shutdown(void);
extern void set_kernel_stack(uint32_t stack);
void print_logo(void);

#define SHELL_BUFFER_SIZE 256
#define MAX_HISTORY 10
#define HISTORY_FILE ".history"
#define MAX_FILENAME_SIZE 63
#define MAX_OUTPUT_SIZE 63

/* Shell state variables */
char shell_buffer[SHELL_BUFFER_SIZE];
char history[MAX_HISTORY][SHELL_BUFFER_SIZE];
int history_count = 0;
int history_index = 0;
int buffer_len = 0;
int cursor_pos = 0;
char shell_cwd[128] = "/";

/**
 * @brief List of supported shell commands.
 */
static const char* shell_commands[] = {
    "help", "ls", "cd", "touch", "mkdir", "vix", "cat", "cp", "mv", "rm", 
    "mkcode", "tcc", "cc", "free", "net", "ping", "dns", "reboot", "shutdown", "clear", "music", "hardbass", NULL
};

/**
 * @brief Calculate the current prompt length.
 */
int get_prompt_len() { return 11 + strlen(shell_cwd) + 2; }

/**
 * @brief Redraw the current shell line.
 */
void shell_refresh_line() {
    int prompt_len = get_prompt_len();
    for (int i = prompt_len; i < 80; i++) terminal_putentryat(' ', 0x07, i, terminal_row);
    for (int i = 0; i < buffer_len; i++) terminal_putentryat(shell_buffer[i], 0x07, prompt_len + i, terminal_row);
    update_cursor(prompt_len + cursor_pos, terminal_row);
}

/**
 * @brief Display the shell prompt.
 */
void print_prompt() {
    terminal_setcolor(0x02); terminal_writestring("root@jexos:");
    terminal_setcolor(0x0B); terminal_writestring(shell_cwd);
    terminal_setcolor(0x02); terminal_writestring("> ");
    terminal_setcolor(0x07);
}

/**
 * @brief Tab-completion for commands and filenames.
 */
void shell_autocomplete() {
    if (buffer_len == 0) return;
    char* last_space = strrchr(shell_buffer, ' ');
    char* search_term = last_space ? last_space + 1 : shell_buffer;
    int search_len = strlen(search_term);
    const char* match = NULL;
    int match_count = 0;
    
    /* Search commands */
    if (!last_space) {
        for (int i = 0; shell_commands[i]; i++) {
            if (strncmp(shell_commands[i], search_term, search_len) == 0) {
                match = shell_commands[i]; match_count++;
            }
        }
    }
    
    /* Search filesystem */
    struct jex_inode dir_inode; jexfs_read_inode(cwd_inode, &dir_inode);
    uint8_t buf[BLOCK_SIZE]; read_block(dir_inode.blocks[0], buf);
    struct jex_dir_entry* de = (struct jex_dir_entry*)buf;
    for (unsigned int i = 0; i < DIR_ENTRIES_PER_BLOCK; i++) {
        if (de[i].inode != 0) {
            if (strcmp(de[i].name, ".") == 0 || strcmp(de[i].name, "..") == 0) continue;
            if (strncmp(de[i].name, search_term, search_len) == 0) {
                match = de[i].name; match_count++;
            }
        }
    }
    
    if (match_count == 1 && match) {
        int term_offset = search_term - shell_buffer;
        strcpy(shell_buffer + term_offset, (char*)match);
        buffer_len = strlen(shell_buffer); cursor_pos = buffer_len;
        shell_refresh_line();
    }
}

/**
 * @brief Save shell history to disk.
 */
void shell_save_history() {
    fs_create(HISTORY_FILE);
    int fd = fs_open(HISTORY_FILE, 0);
    if (fd != -1) {
        fs_write(fd, &history_count, sizeof(int));
        for (int i = 0; i < history_count; i++) fs_write(fd, history[i], SHELL_BUFFER_SIZE);
        fs_close(fd);
    }
}

/**
 * @brief Load shell history from disk.
 */
void shell_load_history() {
    int fd = fs_open(HISTORY_FILE, 0);
    if (fd != -1) {
        fs_read(fd, &history_count, sizeof(int));
        if (history_count > MAX_HISTORY) history_count = MAX_HISTORY;
        for (int i = 0; i < history_count; i++) fs_read(fd, history[i], SHELL_BUFFER_SIZE);
        history_index = history_count; fs_close(fd);
    }
}

/**
 * @brief Play a short start-up tune.
 */
void play_tune() {
    beep(392, 100); beep(523, 100); beep(659, 100);
    beep(784, 300); beep(659, 150); beep(784, 400);
}

/**
 * @brief Draw a single frame of the hardbass Gopnik dance animation.
 */
void draw_dance_frame(int frame) {
    terminal_initialize();
    if (frame == 0) {
        terminal_setcolor(0x0E); // Yellow
        terminal_writestring("   ||========================================================||\n");
        terminal_writestring("   ||              JEXOS HARDBASS SECRET PARTY               ||\n");
        terminal_writestring("   ||========================================================||\n\n");
        terminal_setcolor(0x0A); // Light Green
        terminal_writestring("          \\('v')/          \\('v')/          \\('v')/\n");
        terminal_writestring("            | |              | |              | |\n");
        terminal_writestring("           /   \\            /   \\            /   \\\n");
        terminal_writestring("          _\\\\_//_          _\\\\_//_          _\\\\_//_\n\n");
    } else {
        terminal_setcolor(0x0B); // Light Cyan
        terminal_writestring("   ||========================================================||\n");
        terminal_writestring("   ||              JEXOS HARDBASS SECRET PARTY               ||\n");
        terminal_writestring("   ||========================================================||\n\n");
        terminal_setcolor(0x0C); // Light Red
        terminal_writestring("          /('v')\\          /('v')\\          /('v')\\\n");
        terminal_writestring("            | |              | |              | |\n");
        terminal_writestring("           /   \\            /   \\            /   \\\n");
        terminal_writestring("          _//_\\\\_          _//_\\\\_          _//_\\\\_\n\n");
    }
    terminal_setcolor(0x0D); // Light Magenta
    terminal_writestring("               === CHEEKI BREEKI IV DAMKE !!! ===\n");
}

/**
 * @brief Play a Russian Hardbass tune with dancing gopniks.
 */
void play_hardbass() {
    for (int bar = 0; bar < 4; bar++) {
        // Beat 1
        draw_dance_frame(0);
        beep(120, 80); // Bass Kick (F2ish)
        sleep(20);
        beep(587, 80); // Donk (D5)
        sleep(20);
        
        // Beat 2
        draw_dance_frame(1);
        beep(120, 80); // Bass Kick
        sleep(20);
        beep(587, 80); // Donk (D5)
        sleep(20);
        
        // Beat 3
        draw_dance_frame(0);
        beep(120, 80); // Bass Kick
        sleep(20);
        beep(659, 80); // Donk (E5)
        sleep(20);
        
        // Beat 4
        draw_dance_frame(1);
        beep(120, 80); // Bass Kick
        sleep(20);
        beep(698, 80); // Donk (F5)
        sleep(20);
        
        // Beat 5
        draw_dance_frame(0);
        beep(120, 80); // Bass Kick
        sleep(20);
        beep(698, 80); // Donk (F5)
        sleep(20);
        
        // Beat 6
        draw_dance_frame(1);
        beep(120, 80); // Bass Kick
        sleep(20);
        beep(659, 80); // Donk (E5)
        sleep(20);
        
        // Beat 7
        draw_dance_frame(0);
        beep(120, 80); // Bass Kick
        sleep(20);
        beep(587, 80); // Donk (D5)
        sleep(20);
        
        // Beat 8
        draw_dance_frame(1);
        beep(120, 80); // Bass Kick
        sleep(20);
        beep(523, 80); // Donk (C5)
        sleep(20);
    }
    
    // Cleanup and return to normal shell
    terminal_initialize();
    print_logo();
}

/**
 * @brief Convert an integer to a string.
 */
void int_to_string(int n, char* str) {
    int i = 0; int is_neg = 0;
    if (n == 0) { str[0] = '0'; str[1] = '\0'; return; }
    if (n < 0) { is_neg = 1; n = -n; }
    while (n != 0) { str[i++] = (n % 10) + '0'; n /= 10; }
    if (is_neg) str[i++] = '-';
    str[i] = '\0';
    int start = 0; int end = i - 1;
    while (start < end) {
        char temp = str[start]; str[start] = str[end]; str[end] = temp; start++; end--;
    }
}

/**
 * @brief Convert a string to an integer.
 */
int atoi(const char* str) {
    int res = 0;
    for (int i = 0; str[i] != '\0'; ++i) { if (str[i] >= '0' && str[i] <= '9') res = res * 10 + str[i] - '0'; }
    return res;
}

/**
 * @brief Display the JexOS ASCII logo.
 */
void print_logo() {
    terminal_setcolor(0x0B); 
    terminal_writestring("      _             ___  ____  \n");
    terminal_writestring("     | | _____  __ / _ \\ / ___| \n");
    terminal_writestring("  _  | |/ _ \\ \\/ /| | | \\___ \\ \n");
    terminal_writestring(" | |_| |  __/>  < | |_| |___) |\n");
    terminal_writestring("  \\___/ \\___/_/\\_\\ \\___/|____/ \n");
    terminal_setcolor(0x07);
}



/**
 * @brief Parse dotted-decimal IP string (e.g. "10.0.2.2") into uint32_t (net order).
 */
static uint32_t parse_ip(const char* str) {
    uint32_t ip = 0;
    int val = 0;
    int shift = 24;
    while (*str) {
        if (*str == '.') {
            ip |= (val & 0xFF) << shift;
            val = 0;
            shift -= 8;
        } else if (*str >= '0' && *str <= '9') {
            val = val * 10 + (*str - '0');
        } else {
            return 0;
        }
        str++;
    }
    ip |= (val & 0xFF) << shift;
    return htonl(ip);
}

/**
 * @brief Resolve or parse an address — IP string or hostname.
 *
 * Tries dotted-decimal first; if that fails, attempts DNS resolution.
 * Returns 0 if both fail.
 */
static uint32_t resolve_or_parse(const char* str) {
    uint32_t ip = parse_ip(str);
    if (ip != 0)
        return ip;
    terminal_writestring("  Resolving hostname...\n");
    ip = net_dns_resolve(str);
    if (ip == 0)
        terminal_writestring("  DNS resolution failed\n");
    return ip;
}

/**
 * @brief Ping an IP address or hostname.
 */
void ping_command(const char* arg) {
    if (!rtl8139_is_initialized()) {
        terminal_writestring("Network not initialized\n");
        return;
    }
    uint32_t ip = resolve_or_parse(arg);
    if (ip == 0) {
        terminal_writestring("Usage: ping <ip|hostname>   e.g. ping 10.0.2.2\n");
        return;
    }

    int before = net_get_ping_responses();
    int ret = net_ping(ip);

    if (ret < 0) {
        terminal_writestring("  ARP request sent (waiting for address resolution).\n");
        terminal_writestring("  Try again in a moment.\n");
        return;
    }

    /* Busy-wait for a reply (interrupt handler will set ping_responses) */
    for (int i = 0; i < 500000; i++) {
        if (net_get_ping_responses() > before) {
            terminal_writestring("  64 bytes from ");
            terminal_writestring(arg);
            terminal_writestring(": icmp_seq=1\n");
            return;
        }
    }

    terminal_writestring("  No reply received (timeout)\n");
}

/**
 * @brief Resolve a hostname and print its IP.
 */
void dns_command(const char* arg) {
    if (!rtl8139_is_initialized()) {
        terminal_writestring("Network not initialized\n");
        return;
    }
    uint32_t ip = net_dns_resolve(arg);
    if (ip == 0) {
        terminal_writestring("  Not found\n");
        return;
    }
    terminal_writestring("  ");
    terminal_writestring(arg);
    terminal_writestring(" -> ");
    /* Print dotted-decimal */
    uint8_t* bytes = (uint8_t*)&ip;
    char buf[4];
    int_to_string(bytes[0], buf); terminal_writestring(buf); terminal_putchar('.');
    int_to_string(bytes[1], buf); terminal_writestring(buf); terminal_putchar('.');
    int_to_string(bytes[2], buf); terminal_writestring(buf); terminal_putchar('.');
    int_to_string(bytes[3], buf); terminal_writestring(buf);
    terminal_writestring("\n");
}

/**
 * @brief Display network status (MAC, I/O base, IRQ, packet counters).
 */
void net_command() {
    if (!rtl8139_is_initialized()) {
        terminal_writestring("Network: Not initialized\n");
        return;
    }
    uint8_t mac[6];
    rtl8139_get_mac(mac);
    terminal_writestring("RTL8139 Network\n");
    terminal_writestring("  MAC: ");
    for (int i = 0; i < 6; i++) {
        const char* hex = "0123456789ABCDEF";
        terminal_putchar(hex[mac[i] >> 4]);
        terminal_putchar(hex[mac[i] & 0xF]);
        if (i < 5) terminal_putchar(':');
    }
    terminal_writestring("\n");
    terminal_writestring("  IO:  0x");
    uint32_t io = rtl8139_get_io_base();
    const char* hex = "0123456789ABCDEF";
    terminal_putchar(hex[(io >> 12) & 0xF]);
    terminal_putchar(hex[(io >> 8) & 0xF]);
    terminal_putchar(hex[(io >> 4) & 0xF]);
    terminal_putchar(hex[io & 0xF]);
    terminal_writestring("\n");

    terminal_writestring("  IRQ: ");
    char buf[8];
    int irq = rtl8139_get_irq();
    int i = 0;
    if (irq == 0) { buf[i++] = '0'; }
    else { int n = irq; char tmp[8]; int j = 0;
        while (n > 0) { tmp[j++] = '0' + (n % 10); n /= 10; }
        while (j > 0) buf[i++] = tmp[--j]; }
    buf[i] = '\0';
    terminal_writestring(buf);
    terminal_writestring("\n");

    terminal_writestring("  RX:   ");
    int_to_string(rtl8139_get_rx_count(), buf);
    terminal_writestring(buf);
    terminal_writestring(" packets\n");

    terminal_writestring("  TX:   ");
    int_to_string(rtl8139_get_tx_count(), buf);
    terminal_writestring(buf);
    terminal_writestring(" packets\n");
}

/**
 * @brief Display help text.
 */
void help_command() {
    terminal_writestring("Available commands:\n");
    terminal_writestring("  help      - Show this help message\n");
    terminal_writestring("  ls        - List files\n");
    terminal_writestring("  cd <dir>  - Change directory\n");
    terminal_writestring("  touch <f> - Create file\n");
    terminal_writestring("  mkdir <d> - Create directory\n");
    terminal_writestring("  vix <f>   - Open Text Editor\n");
    terminal_writestring("  cat <f>   - Read file\n");
    terminal_writestring("  cp <s> <d>- Copy file\n");
    terminal_writestring("  mv <o> <n>- Rename file\n");
    terminal_writestring("  rm <f>    - Delete file\n");
    terminal_writestring("  mkcode    - Create persistent hello.c\n");
    terminal_writestring("  tcc <f>   - Compile and run C file\n");
    terminal_writestring("  cc <f>    - Compile C file to ELF\n");
    terminal_writestring("  ./<f>     - Execute file\n");
    terminal_writestring("  free      - Show memory usage\n");
    terminal_writestring("  net       - Show network status\n");
    terminal_writestring("  ping <ip> - Ping an IP address or hostname\n");
    terminal_writestring("  dns <host>- Resolve a hostname to IP\n");
    terminal_writestring("  reboot    - Restart JexOS\n");
    terminal_writestring("  shutdown  - Power off JexOS\n");
    terminal_writestring("  music     - Start Jexos Tune\n");
    terminal_writestring("  uptime    - Show system uptime\n");
    terminal_writestring("  dmesg     - Print kernel log buffer\n");
    terminal_writestring("  ps        - List processes\n");
    terminal_writestring("  kill <pid>- Terminate a process\n");
    terminal_writestring("  regs      - Show CPU registers\n");
}

/**
 * @brief Parse and run the current command in the shell buffer.
 */
void execute_command() {
    terminal_writestring("\n"); 
    if (buffer_len > 0) {
        /* Add to history */
        if (history_count < MAX_HISTORY) {
            for(int i=0; i<=buffer_len; i++) history[history_count][i] = shell_buffer[i];
            history_count++;
        } else {
            for (int i = 0; i < MAX_HISTORY - 1; i++) strcpy(history[i], history[i+1]);
            for(int i=0; i<=buffer_len; i++) history[MAX_HISTORY-1][i] = shell_buffer[i];
        }
        history_index = history_count; shell_save_history();
    }
    
    /* Dispatch command */
    if (strcmp(shell_buffer, "help") == 0) help_command();
    else if (strcmp(shell_buffer, "clear") == 0) { terminal_initialize(); print_logo(); }
    else if (strcmp(shell_buffer, "ls") == 0) jexfs_list_dir(cwd_inode);
    else if (strncmp(shell_buffer, "ls ", 3) == 0) {
        int inode = jexfs_open(shell_buffer + 3);
        if (inode != -1) jexfs_list_dir(inode); else terminal_writestring("Directory not found.\n");
    }
    else if (strcmp(shell_buffer, "cd") == 0) { cwd_inode = 1; strcpy(shell_cwd, "/"); }
    else if (strncmp(shell_buffer, "cd ", 3) == 0) {
        char* path = shell_buffer + 3; while(*path == ' ') path++;
        if (strcmp(path, "..") == 0) {
            int inode = jexfs_open("..");
            if (inode != -1) {
                cwd_inode = inode;
                if (strcmp(shell_cwd, "/") != 0) {
                    char* last = strrchr(shell_cwd, '/');
                    if (last == shell_cwd) strcpy(shell_cwd, "/"); else *last = '\0';
                }
            }
        } else {
            int inode = jexfs_open(path);
            if (inode != -1) {
                struct jex_inode ci; jexfs_read_inode(inode, &ci);
                if (ci.mode == 2) {
                    cwd_inode = inode;
                    if (path[0] == '/') strcpy(shell_cwd, path);
                    else { if (strcmp(shell_cwd, "/") != 0) strcat(shell_cwd, "/"); strcat(shell_cwd, path); }
                } else terminal_writestring("Not a directory.\n");
            } else terminal_writestring("Directory not found.\n");
        }
    }
    else if (strncmp(shell_buffer, "touch ", 6) == 0) fs_create(shell_buffer + 6);
    else if (strncmp(shell_buffer, "mkdir ", 6) == 0) jexfs_mkdir(shell_buffer + 6);
    else if (strncmp(shell_buffer, "rm ", 3) == 0) {
        if (jexfs_remove(shell_buffer + 3) < 0) terminal_writestring("Failed to remove file.\n");
    }
    else if (strncmp(shell_buffer, "mv ", 3) == 0) {
        char* old_name = shell_buffer + 3; char* new_name = NULL;
        for (int i = 0; old_name[i] != '\0'; i++) {
            if (old_name[i] == ' ') { old_name[i] = '\0'; new_name = old_name + i + 1; while (*new_name == ' ') new_name++; break; }
        }
        if (new_name && *new_name) {
            if (jexfs_rename(old_name, new_name) < 0) terminal_writestring("Failed to rename file.\n");
        } else terminal_writestring("Usage: mv <old> <new>\n");
    }
    else if (strncmp(shell_buffer, "cp ", 3) == 0) {
        char* src_name = shell_buffer + 3; char* dest_name = NULL;
        for (int i = 0; src_name[i] != '\0'; i++) {
            if (src_name[i] == ' ') { src_name[i] = '\0'; dest_name = src_name + i + 1; while (*dest_name == ' ') dest_name++; break; }
        }
        if (dest_name && *dest_name) {
            int sfd = fs_open(src_name, 0);
            if (sfd < 0) terminal_writestring("Source file not found.\n");
            else {
                fs_create(dest_name); int dfd = fs_open(dest_name, 0);
                if (dfd < 0) terminal_writestring("Failed to create destination.\n");
                else {
                    char buf[512]; int bytes;
                    while ((bytes = fs_read(sfd, buf, 512)) > 0) fs_write(dfd, buf, bytes);
                    fs_close(dfd); terminal_writestring("File copied.\n");
                }
                fs_close(sfd);
            }
        } else terminal_writestring("Usage: cp <src> <dest>\n");
    }
    else if (strncmp(shell_buffer, "cat ", 4) == 0) {
        int fd = fs_open(shell_buffer + 4, 0);
        if (fd != -1) {
            char buf[1024]; int bytes = fs_read(fd, buf, 1023);
            if (bytes > 0) { buf[bytes] = '\0'; terminal_writestring(buf); terminal_writestring("\n"); }
            fs_close(fd);
        } else terminal_writestring("File not found.\n");
    }
    else if (strncmp(shell_buffer, "vix ", 4) == 0) start_editor(shell_buffer + 4);
    else if (strcmp(shell_buffer, "music") == 0) play_tune();
    else if (strcmp(shell_buffer, "hardbass") == 0) play_hardbass();
    else if (strcmp(shell_buffer, "reboot") == 0) reboot();
    else if (strcmp(shell_buffer, "shutdown") == 0) shutdown();
     else if (strcmp(shell_buffer, "poweroff") == 0) shutdown();
    else if (strcmp(shell_buffer, "mkcode") == 0) {
        const char* code = "int main() {\n  printf(\"Hello from JexFS Persistence!\\n\");\n  return 0;\n}";
        fs_create("hello.c"); int fd = fs_open("hello.c", 0); fs_write(fd, code, strlen(code)); fs_close(fd);
        terminal_writestring("hello.c created.\n");
    }
    else if (strncmp(shell_buffer, "tcc ", 4) == 0) {
        char* filename = shell_buffer + 4; while (*filename == ' ') filename++;
        int fd = fs_open(filename, 0);
        if (fd < 0) terminal_writestring("Failed to open file\n");
        else {
            char* source = (char*)kmalloc(4096); int bytes = fs_read(fd, source, 4095);
            source[bytes] = '\0'; fs_close(fd);
            char* argv[] = {filename, NULL}; extern int exec_c_code(const char* c_source, char** argv);
            exec_c_code(source, argv); kfree(source);
        }
    }
    else if (strncmp(shell_buffer, "cc ", 3) == 0) {
        char filename[MAX_FILENAME_SIZE + 1]; char output_file[MAX_OUTPUT_SIZE + 1] = "a.out";
        char* params = shell_buffer + 3; while (*params == ' ') params++;
        int i = 0; while (params[i] && params[i] != ' ' && i < MAX_FILENAME_SIZE) { filename[i] = params[i]; i++; }
        filename[i] = '\0';
        char* next = params + i; while (*next == ' ') next++;
        if (strncmp(next, "-o", 2) == 0) {
            next += 2; while (*next == ' ') next++;
            int j = 0; while (next[j] && next[j] != ' ' && j < MAX_OUTPUT_SIZE) { output_file[j] = next[j]; j++; }
            output_file[j] = '\0';
        }
        int fd = fs_open(filename, 0);
        if (fd < 0) terminal_writestring("Source not found.\n");
        else {
            char* source = (char*)kmalloc(4096); int bytes = fs_read(fd, source, 4095);
            source[bytes] = '\0'; fs_close(fd);
            tcc_state_t* tcc = tcc_new();
            if (tcc && tcc_compile_string(tcc, source) == 0) {
                uint8_t* elf_data; uint32_t elf_size;
                if (tcc_output_memory(tcc, &elf_data, &elf_size) == 0) {
                    fs_create(output_file); int out_fd = fs_open(output_file, 0);
                    fs_write(out_fd, elf_data, elf_size); fs_close(out_fd);
                    terminal_writestring("Compiled: "); terminal_writestring(output_file); terminal_writestring("\n");
                }
            } else terminal_writestring("Compilation failed\n");
            if (tcc) tcc_delete(tcc);
            kfree(source);
        }
    }
    else if (strncmp(shell_buffer, "./", 2) == 0) {
        char* filename = shell_buffer + 2; char* argv[] = {filename, NULL};
        extern int execve_file(const char* filename, char** argv, char** envp);
        execve_file(filename, argv, NULL);
    }
    else if (strcmp(shell_buffer, "free") == 0) {
        char buf[32];
        terminal_writestring("Memory Status:\n  Total: "); int_to_string(pmm_get_total_memory() / 1024, buf); terminal_writestring(buf); terminal_writestring(" KB\n");
        terminal_writestring("  Used:  "); int_to_string(pmm_get_used_memory() / 1024, buf); terminal_writestring(buf); terminal_writestring(" KB\n");
        terminal_writestring("  Free:  "); int_to_string(pmm_get_free_memory() / 1024, buf); terminal_writestring(buf); terminal_writestring(" KB\n");
    }
    else if (strcmp(shell_buffer, "net") == 0) net_command();
    else if (strncmp(shell_buffer, "ping ", 5) == 0) ping_command(shell_buffer + 5);
    else if (strncmp(shell_buffer, "dns ", 4) == 0) dns_command(shell_buffer + 4);
    /* ---- Debug Commands ---- */
    else if (strcmp(shell_buffer, "uptime") == 0) {
        uint32_t ticks = get_ticks();
        uint32_t secs = ticks / TICKS_PER_SEC;
        uint32_t mins = secs / 60;
        uint32_t hours = mins / 60;
        uint32_t days = hours / 24;
        mins %= 60;
        secs %= 60;
        hours %= 24;
        char buf[16];
        terminal_writestring("Uptime: ");
        if (days > 0) {
            int_to_string((int)days, buf);
            terminal_writestring(buf);
            terminal_writestring("d ");
        }
        int_to_string((int)hours, buf);
        terminal_writestring(buf);
        terminal_writestring("h ");
        int_to_string((int)mins, buf);
        terminal_writestring(buf);
        terminal_writestring("m ");
        int_to_string((int)secs, buf);
        terminal_writestring(buf);
        terminal_writestring("s\n");
    }
    else if (strcmp(shell_buffer, "dmesg") == 0) {
        klog_dump();
    }
    else if (strcmp(shell_buffer, "ps") == 0) {
        task_list();
    }
    else if (strncmp(shell_buffer, "kill ", 5) == 0) {
        char* pidstr = shell_buffer + 5;
        int pid = atoi(pidstr);
        if (pid == 1) {
            terminal_writestring("kill: cannot kill init task\n");
        } else if (task_kill(pid) < 0) {
            terminal_writestring("kill: task not found\n");
        } else {
            terminal_writestring("Task marked for termination\n");
        }
    }
    else if (strcmp(shell_buffer, "regs") == 0) {
        uint32_t eax, ebx, ecx, edx, esi, edi, ebp, esp, eflags;
        __asm__ volatile("mov %%eax, %0; mov %%ebx, %1; mov %%ecx, %2; mov %%edx, %3; mov %%esi, %4; mov %%edi, %5"
            : "=m"(eax), "=m"(ebx), "=m"(ecx), "=m"(edx), "=m"(esi), "=m"(edi));
        __asm__ volatile("mov %%ebp, %0; mov %%esp, %1; pushf; pop %2"
            : "=m"(ebp), "=m"(esp), "=m"(eflags));
        char buf[12];
        terminal_writestring("EAX: "); format_hex(eax, buf); terminal_writestring(buf);
        terminal_writestring("  EBX: "); format_hex(ebx, buf); terminal_writestring(buf);
        terminal_writestring("  ECX: "); format_hex(ecx, buf); terminal_writestring(buf);
        terminal_writestring("  EDX: "); format_hex(edx, buf); terminal_writestring(buf);
        terminal_writestring("\n");
        terminal_writestring("ESI: "); format_hex(esi, buf); terminal_writestring(buf);
        terminal_writestring("  EDI: "); format_hex(edi, buf); terminal_writestring(buf);
        terminal_writestring("  EBP: "); format_hex(ebp, buf); terminal_writestring(buf);
        terminal_writestring("  ESP: "); format_hex(esp, buf); terminal_writestring(buf);
        terminal_writestring("\n");
        terminal_writestring("EFLAGS: "); format_hex(eflags, buf); terminal_writestring(buf);
        terminal_writestring("\n");
    }
    else if (shell_buffer[0] != '\0') {
        terminal_writestring("Unknown: "); terminal_writestring(shell_buffer); terminal_writestring("\n");
    }
    
    /* Reset buffer */
    for (int i = 0; i < SHELL_BUFFER_SIZE; i++) shell_buffer[i] = 0;
    buffer_len = 0; cursor_pos = 0;
    print_prompt();
}

/**
 * @brief Main shell initialization and entrance.
 */
void shell_init() {
    shell_load_history();
    print_logo();
    terminal_writestring("\nWelcome to JexOS v0.5 Peak UX Release!\nType 'help' for a list of commands.\n\n");
    print_prompt();
    log_serial("Btw, if you read this, this an staging version!\n");
    log_serial("And cuz no one can try the staging unless your a dev\n");
    log_serial("You're a pretty cute dev, *pat, pat*");
}

/**
 * @brief Shell main loop (handles serial bridge).
 */
void shell_main() {
    shell_init();
    shell_loop();
}

/**
 * @brief Simple shell loop that re-prints the prompt and waits for input.
 * This is used by SYS_EXIT to return to the shell.
 */
void shell_loop() {
    print_prompt();
    extern int is_serial_received(); extern char read_serial();
    while(1) {
        while (is_serial_received()) {
            char c = read_serial(); if (c == '\r') c = '\n'; shell_input(c);
        }
        __asm__ volatile("hlt");
    }
}

/**
 * @brief Processes a single character input for the shell.
 * 
 * Handles backspace, arrows, tab completion, history, and regular typing.
 */
void shell_input(char key) {
    if (editor_running) { editor_input(key); return; }
    if (key == '\n') { execute_command(); return; }
    if (key == '\t') { shell_autocomplete(); return; }
    
    /* Left/Right Arrows */
    if ((unsigned char)key == 0x82) { if (cursor_pos > 0) cursor_pos--; shell_refresh_line(); return; }
    if ((unsigned char)key == 0x83) { if (cursor_pos < buffer_len) cursor_pos++; shell_refresh_line(); return; }
    
    /* Up Arrow (History Back) */
    if ((unsigned char)key == 0x80) {
        if (history_count > 0) {
            if (history_index > 0) history_index--;
            int i = 0; while(history[history_index][i] && i < SHELL_BUFFER_SIZE-1) { shell_buffer[i] = history[history_index][i]; i++; }
            shell_buffer[i] = 0; buffer_len = i; cursor_pos = i; shell_refresh_line();
        }
        return;
    }
    
    /* Down Arrow (History Forward) */
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
        /* Support character insertion in the middle of the buffer */
        if (cursor_pos < buffer_len) { for (int i = buffer_len; i > cursor_pos; i--) shell_buffer[i] = shell_buffer[i-1]; }
        shell_buffer[cursor_pos] = key; buffer_len++; cursor_pos++; shell_buffer[buffer_len] = 0; shell_refresh_line();
    }
}
