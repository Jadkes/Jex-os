/**
 * @file rtl8139.h
 * @brief RTL8139 Network Card Driver.
 *
 * Implements basic RX/TX for the Realtek 8139 NIC.
 */

#ifndef RTL8139_H
#define RTL8139_H

#include <stdint.h>
#include "pci.h"

/* RTL8139 Register Offsets */
#define RTL8139_REG_MAC0        0x00
#define RTL8139_REG_MAR0        0x08
#define RTL8139_REG_TSD0        0x10
#define RTL8139_REG_TSAD0       0x20
#define RTL8139_REG_RBSTART     0x30
#define RTL8139_REG_CR          0x37
#define RTL8139_REG_CAPR        0x38
#define RTL8139_REG_IMR         0x3C
#define RTL8139_REG_ISR         0x3E
#define RTL8139_REG_TCR         0x40
#define RTL8139_REG_RCR         0x44
#define RTL8139_REG_MPC         0x4C
#define RTL8139_REG_CONFIG1     0x52

/* Command Register Bits */
#define RTL8139_CR_BUFE         (1 << 0)
#define RTL8139_CR_TE           (1 << 2)
#define RTL8139_CR_RE           (1 << 3)
#define RTL8139_CR_RST          (1 << 4)

/* Interrupt Status/Mask Register Bits */
#define RTL8139_ISR_ROK         (1 << 0)
#define RTL8139_ISR_TER         (1 << 1)
#define RTL8139_ISR_TOK         (1 << 2)
#define RTL8139_ISR_RER         (1 << 3)

/* Receive Configuration Register Bits */
#define RTL8139_RCR_AAP         (1 << 0) // Accept All Packets
#define RTL8139_RCR_APM         (1 << 1) // Accept Physical Match
#define RTL8139_RCR_AM          (1 << 2) // Accept Multicast
#define RTL8139_RCR_AB          (1 << 3) // Accept Broadcast
#define RTL8139_RCR_WRAP        (1 << 7) // Wrap bit

/**
 * @brief Initialize the RTL8139 driver.
 * Scans the PCI bus for the device and configures registers for operation.
 * 
 * @return void
 */
void init_rtl8139();

/**
 * @brief Send a network packet.
 * Copies the data to a transmit buffer and triggers the DMA transfer.
 * 
 * @param data Pointer to the packet data.
 * @param len Length of the packet in bytes.
 * @return void
 */
void rtl8139_send_packet(void* data, uint32_t len);

/**
 * @brief Get the driver initialization status.
 * @return 1 if initialized, 0 otherwise.
 */
int rtl8139_is_initialized(void);

/**
 * @brief Copy the MAC address into the provided buffer.
 * @param mac 6-byte buffer to receive the MAC address.
 */
void rtl8139_get_mac(uint8_t* mac);

/**
 * @brief Get the I/O base port address.
 * @return The I/O base address.
 */
uint32_t rtl8139_get_io_base(void);

/**
 * @brief Get the assigned IRQ number.
 * @return The IRQ line number.
 */
uint8_t rtl8139_get_irq(void);

/**
 * @brief Get the number of packets received.
 * @return Packet RX count.
 */
uint32_t rtl8139_get_rx_count(void);

/**
 * @brief Get the number of packets transmitted.
 * @return Packet TX count.
 */
uint32_t rtl8139_get_tx_count(void);

/**
 * @brief Dump RTL8139 register state to the terminal for debugging.
 */
void rtl8139_dump_regs(void);

#endif // RTL8139_H
