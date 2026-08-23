#ifndef SERIAL_H
#define SERIAL_H

#include <stdint.h>
#include <config.h>

/* 16550 UART register offsets from SERIAL_COM1_BASE */
#define UART_DATA       0   /* transmit/receive buffer (DLAB=0) */
#define UART_IER        1   /* interrupt enable        (DLAB=0) */
#define UART_FIFO_CTRL  2
#define UART_LINE_CTRL  3
#define UART_MODEM_CTRL 4
#define UART_LINE_STAT  5

/* UART_LINE_CTRL bits */
#define UART_LCR_DLAB   0x80    /* divisor latch access     */
#define UART_LCR_8N1    0x03    /* 8 data, no parity, 1 stop */

/* UART_LINE_STAT bits */
#define UART_LSR_DATA_READY   0x01
#define UART_LSR_THRE_EMPTY   0x20

/* Divisor for a 38400 baud line: 115200 / 38400 = 3 */
#define UART_BAUD_DIVISOR 3

/**
 * @brief Initialize the serial port (COM1).
 * Configures for 115200 baud, 8 data bits, no parity, 1 stop bit.
 */
void init_serial(void);

/**
 * @brief Write a single character to the serial port.
 * Blocks until the transmitter is ready.
 * @param a The character to write.
 */
void write_serial(char a);

/**
 * @brief Read a single character from the serial port.
 * Blocks until a character is available.
 * @return The character read.
 */
char read_serial(void);

/**
 * @brief Check if data is available to read from serial.
 * @return Non-zero if data is available, 0 otherwise.
 */
int is_serial_received(void);

/**
 * @brief Check if the transmitter is ready to accept a new byte.
 * @return Non-zero if ready, 0 otherwise.
 */
int is_transmit_empty(void);

/**
 * @brief Write a null-terminated string to the serial port.
 * @param str The string to write.
 */
void log_serial(const char* str);

/**
 * @brief Write a 32-bit integer as a hexadecimal string to serial.
 * @param n The number to write.
 */
void log_hex_serial(uint32_t n);

#endif // SERIAL_H
