/**
 * @file dump.h
 * @brief Hexdump memory utility for kernel debugging.
 *
 * Purpose: Provide a safe way to inspect arbitrary memory regions
 *          from the shell, with page-fault protection.
 */

#ifndef DUMP_H
#define DUMP_H

#include <stdint.h>

/**
 * @brief Hexdump 'len' bytes starting at 'addr'.
 *
 * Prints address-offset hex + ASCII lines to the terminal.
 * Default len is 128, clamped to max 1024.
 * No page-fault protection (caller must pass valid addresses).
 *
 * @param addr  Starting virtual address.
 * @param len   Number of bytes to dump (clamped 0-1024).
 */
void hexdump(uint32_t addr, uint32_t len);

#endif /* DUMP_H */
