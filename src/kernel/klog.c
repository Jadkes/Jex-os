/**
 * @file klog.c
 * @brief Kernel Ring Buffer (klog) — captures all log_serial() output
 *
 * Purpose: Provides a 4 KB ring buffer that stores log output for later
 *          retrieval via the dmesg command. All log_serial() calls feed
 *          into this buffer automatically.
 * Thread-safety: The buffer uses volatile for the write position, but is
 *                not yet safe for concurrent writers (single-core for now).
 */

#include "klog.h"
#include "terminal.h"

static char klog_buf[KLOG_SIZE];
static volatile uint32_t klog_pos = 0;
static int klog_level = KLOG_LEVEL_INFO;

void klog_init(void)
{
    klog_pos = 0;
    for (uint32_t i = 0; i < KLOG_SIZE; i++)
        klog_buf[i] = 0;
}

void klog_write(int level, const char* str)
{
    if (!str)
        return;
    if (level > klog_level)
        return;    /* Drop messages above configured severity (lower = more severe) */
    for (uint32_t i = 0; str[i] != '\0'; i++) {
        klog_buf[klog_pos % KLOG_SIZE] = str[i];
        klog_pos++;
    }
    klog_buf[klog_pos % KLOG_SIZE] = '\0';
}

void klog_dump(void)
{
    uint32_t end = klog_pos < KLOG_SIZE ? klog_pos : KLOG_SIZE;
    uint32_t start = (klog_pos < KLOG_SIZE) ? 0 : (klog_pos % KLOG_SIZE);

    /* Always use putchar to avoid mutating the buffer (read-only dump) */
    for (uint32_t i = start; i < end; i++) {
        if (klog_buf[i])
            terminal_putchar(klog_buf[i]);
    }

    /* If buffer wrapped, emit the remaining data from 0 to start */
    if (klog_pos >= KLOG_SIZE) {
        for (uint32_t i = 0; i < start; i++) {
            if (klog_buf[i])
                terminal_putchar(klog_buf[i]);
        }
    }
    terminal_writestring("\n");
}

void klog_set_level(int level)
{
    if (level < KLOG_LEVEL_ERROR) level = KLOG_LEVEL_ERROR;
    if (level > KLOG_LEVEL_DEBUG) level = KLOG_LEVEL_DEBUG;
    klog_level = level;
}

int klog_get_level(void)
{
    return klog_level;
}
