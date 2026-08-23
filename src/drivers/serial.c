#include <serial.h>
#include <ports.h>
#include <config.h>
#include "klog.h"

/**
 * @file serial.c
 * @brief Serial port (COM1) driver implementation.
 * Used for debugging output and host-guest communication.
 */

/*
 * init_serial - set up COM1 at 38400 baud, 8N1, FIFO on
 */
void init_serial() {
   outb(SERIAL_COM1_BASE + UART_IER, 0x00);
   outb(SERIAL_COM1_BASE + UART_LINE_CTRL, UART_LCR_DLAB);
   outb(SERIAL_COM1_BASE + UART_DATA,
        UART_BAUD_DIVISOR & 0xFF);
   outb(SERIAL_COM1_BASE + UART_IER,
        (UART_BAUD_DIVISOR >> 8) & 0xFF);
   outb(SERIAL_COM1_BASE + UART_LINE_CTRL, UART_LCR_8N1);
   /* Enable and clear FIFOs, 14-byte receive threshold */
   outb(SERIAL_COM1_BASE + UART_FIFO_CTRL, 0xC7);
   /* DTR + RTS + OUT2 (gate the UART interrupt line) */
   outb(SERIAL_COM1_BASE + UART_MODEM_CTRL, 0x0B);
}

/*
 * is_transmit_empty - can another byte be queued for transmit
 */
int is_transmit_empty() {
   return inb(SERIAL_COM1_BASE + UART_LINE_STAT) & UART_LSR_THRE_EMPTY;
}

/*
 * write_serial - blocking single-character transmit
 * @a: character to send
 */
void write_serial(char a) {
   while (is_transmit_empty() == 0);
   outb(SERIAL_COM1_BASE + UART_DATA, a);
}

/*
 * is_serial_received - unread byte waiting in the FIFO
 */
int is_serial_received() {
   return inb(SERIAL_COM1_BASE + UART_LINE_STAT) & UART_LSR_DATA_READY;
}

/*
 * read_serial - blocking single-character read
 */
char read_serial() {
   while (is_serial_received() == 0);
   return inb(SERIAL_COM1_BASE + UART_DATA);
}

/**
 * @brief Writes a null-terminated string to the serial port.
 * @param str The string to write.
 */
void log_serial(const char* str) {
    for (int i = 0; str[i] != '\0'; i++) write_serial(str[i]);
    klog_write(KLOG_LEVEL_INFO, str);
}

/**
 * @brief Writes a 32-bit integer in hexadecimal format to the serial port.
 * @param n The integer to write.
 */
void log_hex_serial(uint32_t n) {
    const char *digits = "0123456789ABCDEF";
    log_serial("0x");
    for (int i = 28; i >= 0; i -= 4) write_serial(digits[(n >> i) & 0xF]);
    log_serial("\n");
}
