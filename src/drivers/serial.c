#include <serial.h>
#include <ports.h>
#include <config.h>

/**
 * @file serial.c
 * @brief Serial port (COM1) driver implementation.
 * Used for debugging output and host-guest communication.
 */

/**
 * @brief Initializes the COM1 serial port.
 * Sets the baud rate to 38400, 8 bits, no parity, one stop bit.
 */
void init_serial() {
   outb(SERIAL_COM1_BASE + 1, 0x00);    // Disable all interrupts
   outb(SERIAL_COM1_BASE + 3, 0x80);    // Enable DLAB (set baud rate divisor)
   outb(SERIAL_COM1_BASE + 0, 0x03);    // Set divisor to 3 (lo byte) 38400 baud
   outb(SERIAL_COM1_BASE + 1, 0x00);    //                  (hi byte)
   outb(SERIAL_COM1_BASE + 3, 0x03);    // 8 bits, no parity, one stop bit
   outb(SERIAL_COM1_BASE + 2, 0xC7);    // Enable FIFO, clear them, with 14-byte threshold
   outb(SERIAL_COM1_BASE + 4, 0x0B);    // IRQs enabled, RTS/DSR set
}

/**
 * @brief Checks if the transmit holding register is empty.
 * @return Non-zero if empty, zero otherwise.
 */
int is_transmit_empty() {
   return inb(SERIAL_COM1_BASE + 5) & 0x20;
}

/**
 * @brief Writes a single character to the serial port.
 * @param a The character to write.
 */
void write_serial(char a) {
   while (is_transmit_empty() == 0);
   outb(SERIAL_COM1_BASE, a);
}

/**
 * @brief Checks if data has been received on the serial port.
 * @return Non-zero if data is available, zero otherwise.
 */
int is_serial_received() {
   return inb(SERIAL_COM1_BASE + 5) & 1;
}

/**
 * @brief Reads a single character from the serial port.
 * @return The character read.
 */
char read_serial() {
   while (is_serial_received() == 0);
   return inb(SERIAL_COM1_BASE);
}

/**
 * @brief Writes a null-terminated string to the serial port.
 * @param str The string to write.
 */
void log_serial(const char* str) {
    for (int i = 0; str[i] != '\0'; i++) write_serial(str[i]);
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
