/*
 * @file shell.c
 * @brief Interactive Kernel Shell.
 *
 * Provides a cli interface for the user to interact with the OS.
 * Supports file management, program execution, and basic system control.
 * And buggy as hell.
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
#include "kernel/backtrace.h"
#include "kernel/workqueue.h"
#include "klog.h"
#include "timer.h"
#include "task.h"
#include "dump.h"
#include "test_suite.h"
#include "tcp.h"
#include "dhcp.h"
#include "debug/ftrace.h"
#include "kernel/vsnprintf.h"
#include <stddef.h>
#include <stdint.h>

/* Forward declarations */
extern void jump_to_user_mode(uint32_t entry, uint32_t stack);
extern void editor_input(char key);
extern int editor_running;
extern void beep(int freq, int duration);
extern void start_editor(const char* filename);
extern int  rtl8139_poll_rx(void);
extern void sleep(uint32_t ms);
extern void reboot(void);
extern void shutdown(void);
extern void set_kernel_stack(uint32_t stack);
void int_to_string(int n, char* str);
void print_logo(void);

#define SHELL_BUFFER_SIZE 256
#define HIST_SIZE 16
#define HISTORY_FILE ".history"
#define MAX_FILENAME_SIZE 63
#define MAX_OUTPUT_SIZE 63

/* VGA text-mode color attribute values (foreground on black background).
 * Attribute byte = (background << 4) | foreground; the constants below
 * are the foreground nibble with background = 0 (black).
 */
#define VGA_COLOR_BLACK          0x00
#define VGA_COLOR_BLUE           0x01
#define VGA_COLOR_GREEN          0x02
#define VGA_COLOR_CYAN           0x03
#define VGA_COLOR_RED            0x04
#define VGA_COLOR_MAGENTA        0x05
#define VGA_COLOR_BROWN          0x06
#define VGA_COLOR_LIGHT_GRAY     0x07
#define VGA_COLOR_DARK_GRAY      0x08
#define VGA_COLOR_LIGHT_BLUE     0x09
#define VGA_COLOR_LIGHT_GREEN    0x0A
#define VGA_COLOR_LIGHT_CYAN     0x0B
#define VGA_COLOR_LIGHT_RED      0x0C
#define VGA_COLOR_LIGHT_MAGENTA  0x0D
#define VGA_COLOR_YELLOW         0x0E
#define VGA_COLOR_WHITE          0x0F

/* Shell UI constants */
#define PROMPT_LEN          9
#define SCREEN_COLS        80
#define MORE_LINE_LIMIT    22

/* Default QEMU gateway octets (10.0.2.2) */
#define DEFAULT_GW_OCTETS  ((10 << 24) | (0 << 16) | (2 << 8) | 2)

/* Timing intervals (timer runs at 100 Hz = 10 ms per tick) */
#define ARP_TIMEOUT_TICKS     1000  /* ~10 seconds */
#define PING_REPLY_TICKS       200  /* ~2 seconds */
#define LOOPBACK_PING_TICKS    500  /* ~5 seconds */
#define PING_DELAY_MS          100

/* Capture defaults */
#define TCPDUMP_DEFAULT_COUNT  5
#define TCPDUMP_WAIT_TICKS    500

/* Code compilation buffer */
#define CODE_BUF_SIZE       4096

/* Stack pages for shell main (32 * 4 KB = 128 KB) */
#define STACK_PAGES          32

/* Shell state variables */
char shell_buffer[SHELL_BUFFER_SIZE];
static char history[HIST_SIZE][256];
static int hist_head = 0;
static int hist_count = 0;
static int hist_pos = 0;
int buffer_len = 0;
int cursor_pos = 0;
char shell_cwd[128] = "/";
static char tab_name_buf[32][256];

/**
 * @brief List of supported shell commands.
 */
static const char* shell_commands[] = {
    "help", "ls", "cd", "touch", "mkdir", "vix", "cat", "cp", "mv", "rm",
    "mkcode", "tcc", "cc", "free", "netlog", "net", "ping", "loopback", "dns", "arp", "route", "tcpdump", "nicregs", "fetch", "serve", "dhcp", "history", "top", "reboot", "shutdown", "clear", "music", "dump", "bt", "backtrace", "runtests", "heapcheck", "stackcheck", "ftrace", NULL
};

/**
 * @brief Calculate the current prompt length.
 */
int get_prompt_len() { return PROMPT_LEN; } /* "JexOS:~$ " */

/**
 * @brief Redraw the current shell line.
 */
void shell_refresh_line() {
    int prompt_len = get_prompt_len();
    for (int i = prompt_len; i < SCREEN_COLS; i++) terminal_putentryat(' ', VGA_COLOR_LIGHT_GRAY, i, terminal_row);
    for (int i = 0; i < buffer_len; i++) terminal_putentryat(shell_buffer[i], VGA_COLOR_LIGHT_GRAY, prompt_len + i, terminal_row);
    update_cursor(prompt_len + cursor_pos, terminal_row);
}

/**
 * @brief Display the shell prompt.
 */
void print_prompt() {
    terminal_setcolor(VGA_COLOR_LIGHT_CYAN);
    terminal_writestring("JexOS:~$ ");
    terminal_setcolor(VGA_COLOR_LIGHT_GRAY);
}

/**
 * @brief Tab-completion for commands and filenames.
 *
 * Completes the word at the cursor. Shows all matching options
 * when multiple completions are possible.
 */
void shell_autocomplete() {
    if (buffer_len == 0) return;

    /* Find start of current word (cursor-aware) */
    int word_start = cursor_pos;
    while (word_start > 0 && shell_buffer[word_start - 1] != ' ')
        word_start--;

    int word_len = cursor_pos - word_start;
    const char* matches[32];
    int match_count = 0;

    if (word_start == 0) {
        /* Complete a command name */
        for (int i = 0; shell_commands[i]; i++) {
            if (strncmp(shell_buffer + word_start, shell_commands[i], (size_t)word_len) == 0)
                matches[match_count++] = shell_commands[i];
        }
    } else {
        /* Complete a filename from the current directory */
        struct jex_inode dir_inode;
        jexfs_read_inode(cwd_inode, &dir_inode);
        uint8_t block_buf[BLOCK_SIZE];

        for (int b = 0; b < JEXFS_DIRECT_COUNT; b++) {
            if (dir_inode.direct_blocks[b] == 0) continue;
            read_block(dir_inode.direct_blocks[b], block_buf);

            uint32_t offset = 0;
            while (offset < BLOCK_SIZE) {
                struct jex_dir_entry *de = (struct jex_dir_entry *)(block_buf + offset);
                if (de->inode == 0) break;

                uint16_t esz = sizeof(struct jex_dir_entry) + de->name_len;
                if (offset + esz > BLOCK_SIZE) break;

                /* Skip . and .. */
                if (de->name_len == 1 && de->name[0] == '.') { offset += esz; continue; }
                if (de->name_len == 2 && de->name[0] == '.' && de->name[1] == '.') { offset += esz; continue; }

                /* Copy name to tab_name_buf */
                uint16_t name_len = de->name_len;
                if (name_len > 255) name_len = 255;
                memcpy(tab_name_buf[match_count], de->name, name_len);
                tab_name_buf[match_count][name_len] = '\0';

                if (strncmp(tab_name_buf[match_count], shell_buffer + word_start, (size_t)word_len) == 0) {
                    matches[match_count] = tab_name_buf[match_count];
                    match_count++;
                    if (match_count >= 32) break;
                }

                offset += esz;
            }
            if (match_count >= 32) break;
        }
    }

    if (match_count == 1) {
        /* Insert the remainder of the match at the cursor */
        const char* suffix = matches[0] + word_len;
        while (*suffix && buffer_len < SHELL_BUFFER_SIZE - 1) {
            /* Shift content right from cursor */
            for (int i = buffer_len; i > cursor_pos; i--)
                shell_buffer[i] = shell_buffer[i - 1];
            shell_buffer[cursor_pos++] = *suffix++;
            buffer_len++;
        }
        shell_buffer[buffer_len] = '\0';
        shell_refresh_line();
    } else if (match_count > 1) {
        /* Show all matching possibilities */
        terminal_writestring("\n");
        for (int i = 0; i < match_count; i++) {
            terminal_writestring(matches[i]);
            terminal_writestring("  ");
        }
        terminal_writestring("\n");
        print_prompt();
        terminal_writestring(shell_buffer);
        update_cursor(PROMPT_LEN + cursor_pos, terminal_row);
    }
}

/**
 * @brief Add a command to the circular history buffer.
 */
static void hist_add(const char* cmd)
{
    if (cmd[0] == '\0') return;
    int i = 0;
    while (cmd[i] && i < 255) {
        history[hist_head][i] = cmd[i];
        i++;
    }
    history[hist_head][i] = '\0';
    hist_head = (hist_head + 1) % HIST_SIZE;
    if (hist_count < HIST_SIZE) hist_count++;
    hist_pos = hist_count;
}

/**
 * @brief Save shell history to disk.
 */
void shell_save_history() {
    fs_create(HISTORY_FILE);
    int fd = fs_open(HISTORY_FILE, 0);
    if (fd != -1) {
        fs_write(fd, &hist_count, sizeof(int));
        for (int i = 0; i < hist_count; i++) {
            int idx = (hist_head - hist_count + i + HIST_SIZE) % HIST_SIZE;
            fs_write(fd, history[idx], 256);
        }
        fs_close(fd);
    }
}

/**
 * @brief Load shell history from disk.
 */
void shell_load_history() {
    int fd = fs_open(HISTORY_FILE, 0);
    if (fd != -1) {
        int count;
        fs_read(fd, &count, sizeof(int));
        if (count > HIST_SIZE) count = HIST_SIZE;
        for (int i = 0; i < count; i++) {
            char buf[256];
            fs_read(fd, buf, 256);
            buf[255] = '\0';
            /* Add to circular buffer */
            int j = 0;
            while (buf[j] && j < 255) {
                history[hist_head][j] = buf[j];
                j++;
            }
            history[hist_head][j] = '\0';
            hist_head = (hist_head + 1) % HIST_SIZE;
            if (hist_count < HIST_SIZE) hist_count++;
        }
        hist_pos = hist_count;
        fs_close(fd);
    }
}

/**
 * @brief Play a short start-up tune.
 */
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
 * @brief Handle the ftrace shell command.
 * @param args Subcommand and arguments (start|stop|add <filter>|clear|dump)
 */
static void ftrace_command(char* args)
{
    if (!args || args[0] == '\0' || strcmp(args, "start") == 0) {
        ftrace_enable();
        terminal_writestring("ftrace started\n");
    } else if (strcmp(args, "stop") == 0) {
        ftrace_disable();
        terminal_writestring("ftrace stopped\n");
    } else if (strcmp(args, "clear") == 0) {
        ftrace_clear_filters();
        terminal_writestring("ftrace filters cleared\n");
    } else if (strcmp(args, "dump") == 0) {
        ftrace_dump();
    } else if (strncmp(args, "add ", 4) == 0) {
        ftrace_add_filter(args + 4);
    } else {
        terminal_writestring("usage: ftrace start|stop|add <filter>|clear|dump\n");
    }
}

/**
 * @brief Display the JexOS ASCII logo.
 */
void print_logo() {
    terminal_setcolor(VGA_COLOR_LIGHT_CYAN);
    terminal_writestring("      _             ___  ____  \n");
    terminal_writestring("     | | _____  __ / _ \\ / ___| \n");
    terminal_writestring("  _  | |/ _ \\ \\/ /| | | \\___ \\ \n");
    terminal_writestring(" | |_| |  __/>  < | |_| |___) |\n");
    terminal_writestring("  \\___/ \\___/_/\\_\\ \\___/|____/ \n");
    terminal_setcolor(VGA_COLOR_LIGHT_GRAY);
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
 * @brief Ping an IP address or hostname with optional -v for verbose timing.
 *
 * When -v is given, shows ARP lookup results, tick-level timing,
 * and round-trip time in milliseconds. Otherwise prints a concise
 * normal ping output ("64 bytes from ...").
 */
void ping_command(const char* arg) {
    if (!rtl8139_is_initialized()) {
        terminal_writestring("Network not initialized\n");
        return;
    }

    int verbose = 0;
    const char* addr = arg;

    /* Check for -v flag */
    if (strncmp(arg, "-v ", 3) == 0) {
        verbose = 1;
        addr = arg + 3;
        while (*addr == ' ') addr++;
    }

    if (*addr == '\0') {
        terminal_writestring("Usage: ping [-v] <ip|hostname>\n");
        return;
    }

    uint32_t ip = resolve_or_parse(addr);
    if (ip == 0) {
        terminal_writestring("Could not resolve address\n");
        return;
    }

    int before = net_get_ping_responses();
    int ret = net_ping(ip);

    if (ret < 0) {
        if (verbose) {
            terminal_writestring("  ARP lookup: MISS -- sending broadcast request\n");
            terminal_writestring("  Waiting for ARP resolution...\n");
        }
        {
            int arp_timeout = 1000;   /* 10 seconds */
            uint32_t start = get_ticks();
            uint8_t resolved_mac[6];
            while (arp_timeout-- > 0) {
                if (resolve_mac(ip, resolved_mac) == 0) break;
                rtl8139_poll_rx();
                sleep(10);
            }
            uint32_t elapsed = get_ticks() - start;
            if (resolve_mac(ip, resolved_mac) == 0) {
                if (verbose) {
                    char buf[16];
                    terminal_writestring("  MAC resolved in ");
                    int_to_string((int)elapsed, buf);
                    terminal_writestring(buf);
                    terminal_writestring(" ticks\n");
                }
                ret = net_ping(ip);
            } else {
                terminal_writestring("  ARP resolution timed out\n");
                return;
            }
        }
    } else {
        if (verbose) {
            terminal_writestring("  ARP lookup: cached\n");
        }
    }

    if (ret < 0) {
        terminal_writestring("  Ping send failed\n");
        return;
    }

    if (verbose) {
        terminal_writestring("  ICMP echo request sent\n");
    }

    /* Wait for reply (~2 second timeout, polling RX periodically) */
    uint32_t start = get_ticks();
    int timeout = 200;
    while (timeout-- > 0 && net_get_ping_responses() <= before) {
        rtl8139_poll_rx();
        sleep(10);
    }

    if (net_get_ping_responses() > before) {
        uint32_t rtt = get_ticks() - start;
        log_serial("PING: reply received (RTT=");
        log_hex_serial(rtt * 10);
        log_serial(" ms)\n");
        if (verbose) {
            char buf[16];
            terminal_writestring("  ICMP echo reply received after ");
            int_to_string((int)rtt, buf);
            terminal_writestring(buf);
            terminal_writestring(" ticks\n");
            terminal_writestring("  Round-trip time: ");
            int_to_string((int)rtt, buf);
            terminal_writestring(buf);
            terminal_writestring(" ticks (");
            int_to_string((int)(rtt * 10), buf);
            terminal_writestring(buf);
            terminal_writestring(" ms)\n");
        } else {
            terminal_writestring("  64 bytes from ");
            terminal_writestring(addr);
            terminal_writestring(": icmp_seq=1\n");
        }
    } else {
        log_serial("PING: timeout\n");
        terminal_writestring("  No reply received (timeout)\n");
    }
}

/**
 * @brief Loopback — send multiple pings and show timing statistics.
 *
 * Sends N pings to a target IP, measures min/avg/max RTT,
 * and shows aggregate packet loss.
 */
static void loopback_command(const char* arg) {
    if (!rtl8139_is_initialized()) {
        terminal_writestring("Network not initialized\n");
        return;
    }

    int count = 3;
    uint32_t ip = 0;

    /* First argument may be count, then IP */
    const char* p = arg;
    if (*p >= '1' && *p <= '9') {
        count = atoi(p);
        while (*p && *p != ' ') p++;
        while (*p == ' ') p++;
    }

    ip = resolve_or_parse(p);
    if (ip == 0) {
        /* Fall back to QEMU default gateway */
        ip = htonl((10 << 24) | (0 << 16) | (2 << 8) | 2); /* 10.0.2.2 */
    }

    if (count < 1) count = 1;
    if (count > 10) count = 10;

    char buf[16];
    terminal_writestring("loopback: pinging ");
    uint8_t* bytes = (uint8_t*)&ip;
    int_to_string(bytes[0], buf); terminal_writestring(buf); terminal_putchar('.');
    int_to_string(bytes[1], buf); terminal_writestring(buf); terminal_putchar('.');
    int_to_string(bytes[2], buf); terminal_writestring(buf); terminal_putchar('.');
    int_to_string(bytes[3], buf); terminal_writestring(buf);
    terminal_writestring(" ");
    int_to_string(count, buf);
    terminal_writestring(buf);
    terminal_writestring(" times\n");

    int success = 0;
    uint32_t min_rtt = 0xFFFFFFFF;
    uint32_t max_rtt = 0;
    uint32_t total_rtt = 0;

    for (int i = 0; i < count; i++) {
        int before = net_get_ping_responses();
        int ret = net_ping(ip);

        if (ret < 0) {
            int timeout = 1000;
            uint8_t resolved_mac[6];
            while (timeout-- > 0) {
                if (resolve_mac(ip, resolved_mac) == 0) break;
                rtl8139_poll_rx();
                sleep(10);
            }
            if (resolve_mac(ip, resolved_mac) == 0)
                ret = net_ping(ip);
        }

        if (ret >= 0) {
            uint32_t start = get_ticks();
            int ping_timeout = 500;  /* ~5 seconds at 100Hz */
            while (ping_timeout-- > 0) {
                rtl8139_poll_rx();
                if (net_get_ping_responses() > before) {
                    uint32_t rtt = get_ticks() - start;
                    success++;
                    if (rtt < min_rtt) min_rtt = rtt;
                    if (rtt > max_rtt) max_rtt = rtt;
                    total_rtt += rtt;
                    break;
                }
                sleep(10);
            }
        }

        sleep(100);  /* 100ms delay between pings */
    }

    terminal_writestring("\n--- loopback statistics ---\n");
    int_to_string(success, buf);
    terminal_writestring(buf);
    terminal_writestring("/");
    int_to_string(count, buf);
    terminal_writestring(buf);
    terminal_writestring(" packets received\n");

    if (success > 0) {
        uint32_t avg = total_rtt / success;
        terminal_writestring("  min/avg/max = ");
        int_to_string((int)(min_rtt * 10), buf); terminal_writestring(buf);
        terminal_writestring("/");
        int_to_string((int)(avg * 10), buf); terminal_writestring(buf);
        terminal_writestring("/");
        int_to_string((int)(max_rtt * 10), buf); terminal_writestring(buf);
        terminal_writestring(" ms\n");
    }

    int loss = ((count - success) * 100) / count;
    terminal_writestring("  packet loss: ");
    int_to_string(loss, buf);
    terminal_writestring(buf);
    terminal_writestring("%\n");
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
    /* Terminal output for the user */
    terminal_writestring("  ");
    terminal_writestring(arg);
    terminal_writestring(" -> ");
    uint8_t* bytes = (uint8_t*)&ip;
    char buf[4];
    int_to_string(bytes[0], buf); terminal_writestring(buf); terminal_putchar('.');
    int_to_string(bytes[1], buf); terminal_writestring(buf); terminal_putchar('.');
    int_to_string(bytes[2], buf); terminal_writestring(buf); terminal_putchar('.');
    int_to_string(bytes[3], buf); terminal_writestring(buf);
    terminal_writestring("\n");

    /* Also log to serial for automated testing */
    log_serial("DNS: ");
    log_serial(arg);
    log_serial(" -> ");
    log_hex_serial(bytes[0]); log_serial(".");
    log_hex_serial(bytes[1]); log_serial(".");
    log_hex_serial(bytes[2]); log_serial(".");
    log_hex_serial(bytes[3]);
    log_serial("\n");
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

    terminal_writestring("  IP:   ");
    {
        uint8_t* ipb = (uint8_t*)&our_ip;
        int_to_string(ipb[0], buf); terminal_writestring(buf); terminal_putchar('.');
        int_to_string(ipb[1], buf); terminal_writestring(buf); terminal_putchar('.');
        int_to_string(ipb[2], buf); terminal_writestring(buf); terminal_putchar('.');
        int_to_string(ipb[3], buf); terminal_writestring(buf);
    }
    terminal_writestring("\n");

    terminal_writestring("  GW:   ");
    {
        uint8_t* gwb = (uint8_t*)&gateway_ip;
        int_to_string(gwb[0], buf); terminal_writestring(buf); terminal_putchar('.');
        int_to_string(gwb[1], buf); terminal_writestring(buf); terminal_putchar('.');
        int_to_string(gwb[2], buf); terminal_writestring(buf); terminal_putchar('.');
        int_to_string(gwb[3], buf); terminal_writestring(buf);
    }
    terminal_writestring("\n");

    terminal_writestring("  DNS:  ");
    {
        uint8_t* dnsb = (uint8_t*)&dns_server;
        int_to_string(dnsb[0], buf); terminal_writestring(buf); terminal_putchar('.');
        int_to_string(dnsb[1], buf); terminal_writestring(buf); terminal_putchar('.');
        int_to_string(dnsb[2], buf); terminal_writestring(buf); terminal_putchar('.');
        int_to_string(dnsb[3], buf); terminal_writestring(buf);
    }
    terminal_writestring("\n");
}

/**
 * @brief Pager helper — pause after ~22 lines and wait for a keypress.
 */
static void more_prompt(int* line_count)
{
    (*line_count)++;
    if (*line_count >= 22) {
        terminal_writestring("--- more (press key) ---");
        while (!keyboard_has_data())
            __asm__ volatile("pause");
        keyboard_read();                    /* consume the key */
        terminal_writestring("\n");
        *line_count = 0;
    }
}

/**
 * @brief Display help text.
 */
void help_command() {
    int hlp_line = 0;
    terminal_writestring("Available commands:\n");
    more_prompt(&hlp_line);
    terminal_writestring("  help      - Show this help message\n");
    more_prompt(&hlp_line);
    terminal_writestring("  ls        - List files\n");
    more_prompt(&hlp_line);
    terminal_writestring("  cd <dir>  - Change directory\n");
    more_prompt(&hlp_line);
    terminal_writestring("  touch <f> - Create file\n");
    more_prompt(&hlp_line);
    terminal_writestring("  mkdir <d> - Create directory\n");
    more_prompt(&hlp_line);
    terminal_writestring("  vix <f>   - Open Text Editor\n");
    more_prompt(&hlp_line);
    terminal_writestring("  cat <f>   - Read file\n");
    more_prompt(&hlp_line);
    terminal_writestring("  cp <s> <d>- Copy file\n");
    more_prompt(&hlp_line);
    terminal_writestring("  mv <o> <n>- Rename file\n");
    more_prompt(&hlp_line);
    terminal_writestring("  rm <f>    - Delete file\n");
    more_prompt(&hlp_line);
    terminal_writestring("  mkcode    - Create persistent hello.c\n");
    more_prompt(&hlp_line);
    terminal_writestring("  tcc <f>   - Compile and run C file\n");
    more_prompt(&hlp_line);
    terminal_writestring("  cc <f>    - Compile C file to ELF\n");
    more_prompt(&hlp_line);
    terminal_writestring("  ./<f>     - Execute file\n");
    more_prompt(&hlp_line);
    terminal_writestring("  free      - Show memory usage\n");
    more_prompt(&hlp_line);
    terminal_writestring("  net       - Show network status\n");
    more_prompt(&hlp_line);
    terminal_writestring("  netlog <mode> - Set network verbosity (all/arp/ip/off)\n");
    more_prompt(&hlp_line);
    terminal_writestring("  ping [-v] <ip|host> - Ping with optional verbose timing\n");
    more_prompt(&hlp_line);
    terminal_writestring("  loopback [n] [ip] - Multi-ping timing statistics\n");
    more_prompt(&hlp_line);
    terminal_writestring("  tcpdump [n] [arp|ip] - Capture and hexdump packets\n");
    more_prompt(&hlp_line);
    terminal_writestring("  dns <host>- Resolve a hostname to IP\n");
    more_prompt(&hlp_line);
    terminal_writestring("  arp       - Show ARP cache\n");
    more_prompt(&hlp_line);
    terminal_writestring("  nicregs   - Dump RTL8139 NIC registers\n");
    more_prompt(&hlp_line);
    terminal_writestring("  fetch <host> [port] - HTTP GET a URL\n");
    more_prompt(&hlp_line);
    terminal_writestring("  serve     - Start HTTP server on port 80\n");
    more_prompt(&hlp_line);
    terminal_writestring("  dhcp      - Auto-configure IP via DHCP (DORA)\n");
    more_prompt(&hlp_line);
    terminal_writestring("  dump <addr> [n] - Hexdump memory at address\n");
    more_prompt(&hlp_line);
    terminal_writestring("  reboot    - Restart JexOS\n");
    more_prompt(&hlp_line);
    terminal_writestring("  shutdown  - Power off JexOS\n");
    more_prompt(&hlp_line);
    terminal_writestring("  music     - Start Jexos Tune\n");
    more_prompt(&hlp_line);
    terminal_writestring("  uptime    - Show system uptime\n");
    more_prompt(&hlp_line);
    terminal_writestring("  dmesg     - Print kernel log buffer\n");
    more_prompt(&hlp_line);
    terminal_writestring("  ps        - List processes\n");
    more_prompt(&hlp_line);
    terminal_writestring("  kill [-<sig>] <pid> - Send a signal to a process\n");
    more_prompt(&hlp_line);
    terminal_writestring("  kill -l            - List available signals\n");
    more_prompt(&hlp_line);
    terminal_writestring("  regs      - Show CPU registers\n");
    more_prompt(&hlp_line);
    terminal_writestring("  bt/backtrace - Show stack trace from current EBP\n");
    more_prompt(&hlp_line);
    terminal_writestring("  runtests   - Run kernel test suite\n");
    more_prompt(&hlp_line);
    terminal_writestring("  heapcheck  - Show kernel heap statistics\n");
    more_prompt(&hlp_line);
    terminal_writestring("  stackcheck - Show current stack pointer\n");
    more_prompt(&hlp_line);
    terminal_writestring("  ftrace <action> - Function tracer (start|stop|add <f>|clear|dump)\n");
    more_prompt(&hlp_line);
    terminal_writestring("  history    - Show command history\n");
    more_prompt(&hlp_line);
    terminal_writestring("  top        - Show per-task CPU usage\n");
    more_prompt(&hlp_line);
}

/**
 * @brief Wrapper for executing a file via kernel_fork (./ handler).
 *
 * Called in a forked task's context.  Runs execve_file with the filename
 * as the sole argument.  The trampoline calls task_exit upon return.
 */
static void exec_wrapper(void* arg)
{
    char* filename = (char*)arg;
    char* argv[] = {filename, NULL};
    extern int execve_file(const char* filename, char** argv, char** envp);
    execve_file(filename, argv, NULL);
}

/**
 * @brief Wrapper for compiling and running C code via TCC in a forked task.
 *
 * Called in a forked task's context.  Opens the source file, compiles
 * and executes it with TCC, then returns (triggering task_exit via the
 * trampoline).
 */
static void tcc_wrapper(void* arg)
{
    char* filename = (char*)arg;
    int fd = fs_open(filename, 0);
    if (fd < 0) {
        terminal_writestring("Failed to open file\n");
        return;
    }
    char* source = (char*)kmalloc(4096);
    int bytes = fs_read(fd, source, 4095);
    source[bytes] = '\0';
    fs_close(fd);
    char* argv[] = {filename, NULL};
    extern int exec_c_code(const char* c_source, char** argv);
    exec_c_code(source, argv);
    kfree(source);
}

/**
 * @brief Format inode mode bits to a "drwxr-xr-x" string.
 *
 * @param mode Inode mode field
 * @param out  Output buffer (at least 12 bytes)
 */
static void format_mode_str(uint16_t mode, char *out)
{
    /* File type */
    if ((mode & JEXFS_TYPE_MASK) == JEXFS_TYPE_DIR)
        out[0] = 'd';
    else if ((mode & JEXFS_TYPE_MASK) == JEXFS_TYPE_FILE)
        out[0] = '-';
    else
        out[0] = '?';

    /* Owner permissions */
    out[1] = (mode & JEXFS_IRUSR) ? 'r' : '-';
    out[2] = (mode & JEXFS_IWUSR) ? 'w' : '-';
    out[3] = (mode & JEXFS_IXUSR) ? 'x' : '-';
    /* Group permissions */
    out[4] = (mode & JEXFS_IRGRP) ? 'r' : '-';
    out[5] = (mode & JEXFS_IWGRP) ? 'w' : '-';
    out[6] = (mode & JEXFS_IXGRP) ? 'x' : '-';
    /* Other permissions */
    out[7] = (mode & JEXFS_IROTH) ? 'r' : '-';
    out[8] = (mode & JEXFS_IWOTH) ? 'w' : '-';
    out[9] = (mode & JEXFS_IXOTH) ? 'x' : '-';
    out[10] = '\0';
}

/**
 * @brief Format a size value as a string, optionally human-readable.
 *
 * @param size   Size in bytes
 * @param human  Non-zero for human-readable (1.2K, 3.4M)
 * @param out    Output buffer (at least 32 bytes)
 */
static void format_size_str(uint32_t size, int human, char *out)
{
    if (!human) {
        int_to_string((int)size, out);
        return;
    }

    if (size < 1024) {
        int_to_string((int)size, out);
    } else if (size < 1024 * 1024) {
        uint32_t whole = size / 1024;
        uint32_t frac = ((size % 1024) * 10) / 1024;
        char buf[16];
        uint32_t pos = 0;
        int_to_string((int)whole, buf);
        while (buf[pos]) { out[pos] = buf[pos]; pos++; }
        out[pos++] = '.';
        int_to_string((int)frac, buf);
        out[pos++] = buf[0];
        out[pos++] = 'K';
        out[pos] = '\0';
    } else {
        uint32_t whole = size / (1024 * 1024);
        uint32_t frac = ((size % (1024 * 1024)) * 10) / (1024 * 1024);
        char buf[16];
        uint32_t pos = 0;
        int_to_string((int)whole, buf);
        while (buf[pos]) { out[pos] = buf[pos]; pos++; }
        out[pos++] = '.';
        int_to_string((int)frac, buf);
        out[pos++] = buf[0];
        out[pos++] = 'M';
        out[pos] = '\0';
    }
}

/**
 * @brief List directory contents with flags.
 *
 * Supports -a (show hidden), -l (long format), -h (human-readable sizes).
 * Scans directories using variable-length jex_dir_entry format.
 */
static void do_ls(const char *arg)
{
    int show_all = 0;
    int long_fmt = 0;
    int human = 0;
    const char *p = arg;

    /* Parse flags */
    while (*p == '-') {
        p++;
        while (*p && *p != ' ') {
            if (*p == 'a') show_all = 1;
            else if (*p == 'l') long_fmt = 1;
            else if (*p == 'h') human = 1;
            p++;
        }
        while (*p == ' ') p++;
    }

    /* Determine target inode */
    int target_inode;
    if (*p == '\0') {
        target_inode = (int)cwd_inode;
    } else {
        target_inode = jexfs_open(p);
        if (target_inode < 0) {
            terminal_writestring("ls: cannot access '");
            terminal_writestring(p);
            terminal_writestring("': No such file or directory\n");
            return;
        }
    }

    struct jex_inode dir_inode;
    jexfs_read_inode((uint32_t)target_inode, &dir_inode);

    /* Check type */
    if ((dir_inode.mode & JEXFS_TYPE_MASK) != JEXFS_TYPE_DIR) {
        /* Single file: show its info */
        char mode_str[12];
        char size_str[32];
        format_mode_str(dir_inode.mode, mode_str);
        format_size_str(dir_inode.size, human, size_str);
        terminal_writestring(mode_str);
        terminal_writestring("  ");
        terminal_writestring(size_str);
        terminal_writestring("  ");
        terminal_writestring(p);
        terminal_writestring("\n");
        return;
    }

    uint8_t block_buf[BLOCK_SIZE];

    if (!long_fmt) {
        /* Short format: names space-separated */
        for (int b = 0; b < JEXFS_DIRECT_COUNT; b++) {
            if (dir_inode.direct_blocks[b] == 0) continue;
            read_block(dir_inode.direct_blocks[b], block_buf);

            uint32_t offset = 0;
            while (offset < BLOCK_SIZE) {
                struct jex_dir_entry *de = (struct jex_dir_entry *)(block_buf + offset);
                if (de->inode == 0) break;
                if (offset + sizeof(struct jex_dir_entry) > BLOCK_SIZE) break;

                uint16_t esz = sizeof(struct jex_dir_entry) + de->name_len;
                if (offset + esz > BLOCK_SIZE) break;

                int is_dot   = (de->name_len == 1 && de->name[0] == '.');
                int is_dotdot = (de->name_len == 2 && de->name[0] == '.' && de->name[1] == '.');
                if (!show_all && (is_dot || is_dotdot)) {
                    offset += esz;
                    continue;
                }

                /* Print name, append / for directories */
                for (uint16_t i = 0; i < de->name_len; i++)
                    terminal_putchar(de->name[i]);
                if (de->file_type == JEXFS_DIRENT_DIR)
                    terminal_putchar('/');
                terminal_writestring("  ");

                offset += esz;
            }
        }
        terminal_writestring("\n");
    } else {
        /* Long format: mode  size  name */
        for (int b = 0; b < JEXFS_DIRECT_COUNT; b++) {
            if (dir_inode.direct_blocks[b] == 0) continue;
            read_block(dir_inode.direct_blocks[b], block_buf);

            uint32_t offset = 0;
            while (offset < BLOCK_SIZE) {
                struct jex_dir_entry *de = (struct jex_dir_entry *)(block_buf + offset);
                if (de->inode == 0) break;
                if (offset + sizeof(struct jex_dir_entry) > BLOCK_SIZE) break;

                uint16_t esz = sizeof(struct jex_dir_entry) + de->name_len;
                if (offset + esz > BLOCK_SIZE) break;

                int is_dot   = (de->name_len == 1 && de->name[0] == '.');
                int is_dotdot = (de->name_len == 2 && de->name[0] == '.' && de->name[1] == '.');
                if (!show_all && (is_dot || is_dotdot)) {
                    offset += esz;
                    continue;
                }

                /* Read entry inode for metadata */
                struct jex_inode entry_inode;
                jexfs_read_inode(de->inode, &entry_inode);

                char mode_str[12];
                char size_str[32];
                format_mode_str(entry_inode.mode, mode_str);
                format_size_str(entry_inode.size, human, size_str);

                terminal_writestring(mode_str);
                terminal_writestring("  ");
                terminal_writestring(size_str);
                terminal_writestring("  ");

                for (uint16_t i = 0; i < de->name_len; i++)
                    terminal_putchar(de->name[i]);
                terminal_writestring("\n");

                offset += esz;
            }
        }
    }
}

/**
 * @brief Parse and run the current command in the shell buffer.
 */
void execute_command() {
    terminal_writestring("\n");
    if (buffer_len > 0) {
        hist_add(shell_buffer);
        shell_save_history();
    }

    /* Dispatch command */
    if (strcmp(shell_buffer, "help") == 0) help_command();
    else if (strcmp(shell_buffer, "clear") == 0) { terminal_initialize(); print_logo(); }
    else if (strcmp(shell_buffer, "ls") == 0) do_ls("");
    else if (strncmp(shell_buffer, "ls ", 3) == 0) do_ls(shell_buffer + 3);
    else if (strcmp(shell_buffer, "cd") == 0) { cwd_inode = 1; strcpy(shell_cwd, "/"); }
    else if (strncmp(shell_buffer, "cd ", 3) == 0) {
        char* path = shell_buffer + 3; while(*path == ' ') path++;
        if (strcmp(path, "..") == 0) {
            int inode = jexfs_open("..");
            if (inode >= 0) {
                cwd_inode = inode;
                if (strcmp(shell_cwd, "/") != 0) {
                    char* last = strrchr(shell_cwd, '/');
                    if (last == shell_cwd) strcpy(shell_cwd, "/"); else *last = '\0';
                }
            }
        } else {
            int inode = jexfs_open(path);
            if (inode >= 0) {
                struct jex_inode ci; jexfs_read_inode(inode, &ci);
                if (ci.mode == 2) {
                    cwd_inode = inode;
                    if (path[0] == '/') {
                        strncpy(shell_cwd, path, sizeof(shell_cwd) - 1);
                        shell_cwd[sizeof(shell_cwd) - 1] = '\0';
                    } else {
                        size_t cur_len = strlen(shell_cwd);
                        size_t path_len = strlen(path);
                        /* Guard: only append if path + '/' + null fits */
                        if (cur_len + 1 + path_len < sizeof(shell_cwd)) {
                            if (strcmp(shell_cwd, "/") != 0)
                                strcat(shell_cwd, "/");
                            strcat(shell_cwd, path);
                        }
                    }
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
        int pid = kernel_fork(tcc_wrapper, (void*)filename);
        if (pid > 0) {
            int status;
            waitpid(pid, &status, 0);
        } else {
            terminal_writestring("fork failed\n");
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
        char* filename = shell_buffer + 2;
        int pid = kernel_fork(exec_wrapper, (void*)filename);
        if (pid > 0) {
            /* Parent — wait for the child to exit */
            int status;
            waitpid(pid, &status, 0);
        } else {
            terminal_writestring("fork failed\n");
        }
    }
    else if (strcmp(shell_buffer, "free") == 0) {
        char buf[32];
        terminal_writestring("Memory Status:\n  Total: "); int_to_string(pmm_get_total_memory() / 1024, buf); terminal_writestring(buf); terminal_writestring(" KB\n");
        terminal_writestring("  Used:  "); int_to_string(pmm_get_used_memory() / 1024, buf); terminal_writestring(buf); terminal_writestring(" KB\n");
        terminal_writestring("  Free:  "); int_to_string(pmm_get_free_memory() / 1024, buf); terminal_writestring(buf); terminal_writestring(" KB\n");
    }
    else if (strncmp(shell_buffer, "netlog ", 7) == 0) {
        char* arg = shell_buffer + 7;
        if (strcmp(arg, "all") == 0)  netlog_set_flags(NETLOG_ALL);
        else if (strcmp(arg, "arp") == 0) netlog_set_flags(NETLOG_ARP);
        else if (strcmp(arg, "ip") == 0)  netlog_set_flags(NETLOG_IP);
        else if (strcmp(arg, "off") == 0) netlog_set_flags(NETLOG_OFF);
        else terminal_writestring("Usage: netlog <all|arp|ip|off>\n");
    }
    else if (strcmp(shell_buffer, "net") == 0) net_command();
    else if (strncmp(shell_buffer, "ping ", 5) == 0) ping_command(shell_buffer + 5);
    else if (strncmp(shell_buffer, "loopback", 8) == 0) {
        char* loop_arg = shell_buffer + 8;
        while (*loop_arg == ' ') loop_arg++;
        loopback_command(loop_arg);
    }
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
        char* args = shell_buffer + 5;
        while (*args == ' ') args++;

        if (strcmp(args, "-l") == 0) {
            terminal_writestring(" 1 SIGHUP   2 SIGINT    9 SIGKILL\n");
            terminal_writestring("15 SIGTERM 17 SIGCHLD\n");
        } else if (args[0] == '-') {
            /* kill -<sig> <pid> */
            int sig = atoi(args + 1);
            while (*args && *args != ' ') args++;
            while (*args == ' ') args++;
            int pid = atoi(args);
            if (pid <= 0) {
                terminal_writestring("usage: kill -<sig> <pid>\n");
            } else if (pid == 1) {
                terminal_writestring("kill: cannot send signal to init task\n");
            } else if (sys_kill(pid, sig) < 0) {
                terminal_writestring("kill: failed\n");
            }
        } else {
            /* kill <pid> — default SIGTERM */
            int pid = atoi(args);
            if (pid == 1) {
                terminal_writestring("kill: cannot kill init task\n");
            } else if (task_kill(pid) < 0) {
                terminal_writestring("kill: task not found\n");
            } else {
                terminal_writestring("Task terminated\n");
            }
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
    else if (strncmp(shell_buffer, "ftrace", 6) == 0) {
        char* args = shell_buffer + 6;
        while (*args == ' ') args++;
        ftrace_command(args);
    }
    else if (strcmp(shell_buffer, "arp") == 0) {
        arp_dump();
    }
    else if (strcmp(shell_buffer, "route") == 0) {
        route_print();
    }
    else if (strcmp(shell_buffer, "history") == 0) {
        for (int i = 0; i < hist_count; i++) {
            int idx = (hist_head - hist_count + i + HIST_SIZE) % HIST_SIZE;
            char line[8];
            int n = snprintf(line, sizeof(line), "  %d  ", i + 1);
            terminal_write(line, (size_t)n);
            terminal_writestring(history[idx]);
            terminal_putchar('\n');
        }
    }
    else if (strcmp(shell_buffer, "top") == 0) {
        uint32_t total = 0;
        task_t* task = (task_t*)ready_queue;

        while (task) {
            total += task->cpu_ticks;
            task = task->next;
        }
        if (current_task)
            total += current_task->cpu_ticks;

        if (total == 0) total = 1;

        terminal_writestring("PID  NAME       CPU%  STATE\n");
        task = (task_t*)ready_queue;
        while (task) {
            uint32_t pct = (task->cpu_ticks * 100) / total;
            char buf[64];
            snprintf(buf, sizeof(buf), "%-3d  %-10s %-4u  %s\n",
                     task->id, task->name, pct, "RUNNING");
            terminal_writestring(buf);
            task = task->next;
        }
        /* Also show current_task if not in ready_queue */
        if (current_task) {
            int found = 0;
            task = (task_t*)ready_queue;
            while (task) { if (task == current_task) { found = 1; break; } task = task->next; }
            if (!found) {
                uint32_t pct = (current_task->cpu_ticks * 100) / total;
                char buf[64];
                snprintf(buf, sizeof(buf), "%-3d  %-10s %-4u  %s\n",
                         current_task->id, current_task->name, pct, "RUNNING");
                terminal_writestring(buf);
            }
        }
    }
    else if (strncmp(shell_buffer, "tcpdump", 7) == 0) {
        int count = 5;
        int filter = TCPDUMP_ALL;
        char* arg = shell_buffer + 7;
        while (*arg == ' ') arg++;
        if (*arg >= '1' && *arg <= '9') {
            count = atoi(arg);
            while (*arg && *arg != ' ') arg++;
            while (*arg == ' ') arg++;
        }
        if (strcmp(arg, "arp") == 0) filter = TCPDUMP_ARP;
        else if (strcmp(arg, "ip") == 0) filter = TCPDUMP_IP;

        if (tcpdump_start(count, filter) == 0) {
            terminal_writestring("Capturing ");
            char buf[4];
            int_to_string(count, buf);
            terminal_writestring(buf);
            terminal_writestring(" packets...\n");

            int timeout = 500;
            while (timeout-- > 0 && !tcpdump_is_done()) {
                sleep(10);
            }
            if (tcpdump_get_count() == 0) {
                terminal_writestring("No packets captured\n");
            } else {
                tcpdump_print();
            }
        }
    }
    else if (strcmp(shell_buffer, "nicregs") == 0) {
        rtl8139_dump_regs();
    }
    else if (strncmp(shell_buffer, "fetch ", 6) == 0) {
        char* arg = shell_buffer + 6;
        while (*arg == ' ') arg++;
        /* Parse hostname [port] */
        char hostname[128];
        uint16_t port = 80;
        uint32_t i = 0;
        while (*arg && *arg != ' ' && i < sizeof(hostname) - 1)
            hostname[i++] = *arg++;
        hostname[i] = '\0';
        while (*arg == ' ') arg++;
        if (*arg >= '1' && *arg <= '9')
            port = (uint16_t)atoi(arg);

        log_serial("FETCH: host=");
        log_serial(hostname);
        log_serial(" port=");
        log_hex_serial(port);
        log_serial("\n");

        http_get(hostname, port, "/");
    }
    else if (strcmp(shell_buffer, "serve") == 0) {
        http_serve(80);
    }
    else if (strcmp(shell_buffer, "dhcp") == 0) {
        dhcp_start();
    }
    else if (strncmp(shell_buffer, "dump ", 5) == 0) {
        char* args = shell_buffer + 5;
        uint32_t addr = 0;
        uint32_t count = 128;
        int parsed_addr = 0;

        if (args[0] == '0' && (args[1] == 'x' || args[1] == 'X')) {
            args += 2;
            while (*args) {
                uint8_t nibble;
                if (*args >= '0' && *args <= '9') nibble = *args - '0';
                else if (*args >= 'a' && *args <= 'f') nibble = *args - 'a' + 10;
                else if (*args >= 'A' && *args <= 'F') nibble = *args - 'A' + 10;
                else break;
                addr = (addr << 4) | nibble;
                args++;
                parsed_addr = 1;
            }
            while (*args == ' ') args++;
            if (*args >= '1' && *args <= '9')
                count = (uint32_t)atoi(args);
        }

        if (!parsed_addr) {
            terminal_writestring("Usage: dump <addr> [bytes]\n");
            terminal_writestring("  e.g. dump 0xC0100000 256\n");
        } else {
            hexdump(addr, count);
        }
    }
    else if (strcmp(shell_buffer, "runtests") == 0) {
        run_all_tests();
    }
    else if (strcmp(shell_buffer, "heapcheck") == 0) {
        char buf[16];
        uint32_t used = kheap_get_used();
        uint32_t free = kheap_get_free();
        terminal_writestring("Heap: slab allocator\n");
        terminal_writestring("  Committed: ");
        int_to_string((int)used, buf);
        terminal_writestring(buf);
        terminal_writestring(" bytes\n  Free:      ");
        int_to_string((int)free, buf);
        terminal_writestring(buf);
        terminal_writestring(" bytes\n");
    }
    else if (strcmp(shell_buffer, "stackcheck") == 0) {
        uint32_t esp;
        __asm__ volatile("mov %%esp, %0" : "=r"(esp));
        char buf[12];
        terminal_writestring("Stack:\n");
        terminal_writestring("  ESP: 0x");
        format_hex(esp, buf);
        terminal_writestring(buf);
        terminal_writestring("\n");
    }
    else if (strcmp(shell_buffer, "bt") == 0 || strcmp(shell_buffer, "backtrace") == 0) {
        uint32_t ebp, eip_frames[MAX_BACKTRACE_DEPTH];
        __asm__ volatile("mov %%ebp, %0" : "=r"(ebp));
        int depth = unwind_stack(ebp, eip_frames, MAX_BACKTRACE_DEPTH);
        if (depth == 0) {
            terminal_writestring("No stack trace available\n");
        } else {
            char buf[12];
            terminal_writestring("Stack Trace:\n");
            for (int i = 0; i < depth; i++) {
                terminal_writestring("  ");
                format_hex(eip_frames[i], buf);
                terminal_writestring(buf);
                terminal_writestring("\n");
            }
        }
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
    terminal_writestring("\nWelcome to JexOS v0.7.0!\nType 'help' for a list of commands.\n\n");
    print_prompt();
}

/**
 * @brief Shell main loop (handles serial bridge).
 *
 * PID 1 starts on the BSS stack (16 KB at 0x159000), which is too small
 * for TCC inline compilation (cc command) — deep recursion in
 * tcc_compile_string plus a 4 KB code_buffer overflows into BSS globals
 * (current_task, ready_queue), corrupting them and causing a triple fault.
 *
 * We allocate a 128 KB stack from PMM (identity-mapped) and switch to it
 * before entering the shell loop.  The BSS stack is abandoned — the old
 * kernel_main frame stays there but is never revisited because shell_loop
 * never returns.
 */
void shell_main(void) {
    void *pstack = pmm_alloc_blocks(32);           /* 128 KB = 32 x 4 KB */
    uint32_t new_esp;

    if (pstack) {
        new_esp = (uint32_t)pstack + (32 * 4096);  /* top of allocation  */
        new_esp &= ~15;                              /* 16-byte alignment */

        __asm__ volatile(
            "movl %0, %%esp\n\t"
            "movl %%esp, %%ebp\n\t"
            :: "r"(new_esp) : "memory"
        );
    }

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
        workqueue_run();
        /* Poll keyboard ring buffer (PS/2 driver writes to ring buffer in ISR) */
        while (keyboard_has_data()) {
            int c = keyboard_read();
            if (c != -1) shell_input((char)c);
        }
        /* Poll serial input for debug/remote access */
        while (is_serial_received()) {
            char c = read_serial(); if (c == '\r') c = '\n'; shell_input(c);
        }
        __asm__ volatile("sti; hlt");  /* ensure interrupts enabled before halt */
    }
}

/**
 * @brief Processes a single character input for the shell.
 * 
 * Handles backspace, arrows, tab completion, history, and regular typing.
 */
void shell_input(char key) {
    if (editor_running) { editor_input(key); return; }

    /* Ctrl+L — clear screen and redraw prompt */
    if (key == 0x0C) {
        terminal_initialize();
        print_logo();
        print_prompt();
        shell_refresh_line();
        return;
    }

    if (key == '\n') { execute_command(); return; }
    if (key == '\t') { shell_autocomplete(); return; }
    
    /* Left/Right Arrows */
    if ((unsigned char)key == 0x82) { if (cursor_pos > 0) cursor_pos--; shell_refresh_line(); return; }
    if ((unsigned char)key == 0x83) { if (cursor_pos < buffer_len) cursor_pos++; shell_refresh_line(); return; }
    
    /* Up Arrow (History Back) */
    if ((unsigned char)key == 0x80) {
        if (hist_count > 0 && hist_pos > 0) {
            hist_pos--;
            int idx = (hist_head - hist_count + hist_pos + HIST_SIZE) % HIST_SIZE;
            int i = 0;
            while (history[idx][i] && i < SHELL_BUFFER_SIZE - 1) {
                shell_buffer[i] = history[idx][i];
                i++;
            }
            shell_buffer[i] = '\0';
            buffer_len = i;
            cursor_pos = i;
            shell_refresh_line();
        }
        return;
    }

    /* Down Arrow (History Forward) */
    if ((unsigned char)key == 0x81) {
        if (hist_count > 0 && hist_pos < hist_count) {
            hist_pos++;
            if (hist_pos == hist_count) {
                shell_buffer[0] = '\0';
                buffer_len = 0;
                cursor_pos = 0;
            } else {
                int idx = (hist_head - hist_count + hist_pos + HIST_SIZE) % HIST_SIZE;
                int i = 0;
                while (history[idx][i] && i < SHELL_BUFFER_SIZE - 1) {
                    shell_buffer[i] = history[idx][i];
                    i++;
                }
                shell_buffer[i] = '\0';
                buffer_len = i;
                cursor_pos = i;
            }
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
