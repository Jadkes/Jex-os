/**
 * @file rtl8139.c
 * @brief RTL8139 Network Card Driver Implementation.
 * 
 * This driver provides basic initialization, packet transmission,
 * and interrupt-driven packet reception for the Realtek 8139.
 * 
 * The RTL8139 uses DMA (Direct Memory Access) to transfer packets
 * between system RAM and its internal FIFO buffers.
 */

#include "rtl8139.h"
#include "ports.h"
#include "pmm.h"
#include "irq.h"
#include "terminal.h"
#include "serial.h"
#include "config.h"
#include <string.h>

#if 0
/* Helper: Convert uint32_t to hex string */
static void int_to_hex(uint32_t val, char* buf) {
    const char* hex = "0123456789ABCDEF";
    buf[0] = '0'; buf[1] = 'x';
    for (int i = 7; i >= 0; i--) {
        buf[2 + (7 - i)] = hex[(val >> (i * 4)) & 0xF];
    }
    buf[10] = '\0';
}
#endif

/* I/O Base Address and IRQ assigned by the PCI bus */
static uint32_t io_base;
#if 0
static uint8_t irq_num;

/* MAC Address read from the card's EEPROM */
static uint8_t mac_addr[6];
#endif

#if 0
/* Receive Buffer: The card writes incoming packets here via DMA */
#define RX_BUF_SIZE (32 * 1024)
static uint8_t* rx_buffer;
#endif

/* Transmit Descriptors index (0 to 3)
 * The RTL8139 has 4 transmit descriptors that can be used concurrently.
 */
static uint32_t current_tx_desc = 0;

/**
 * @brief RTL8139 Interrupt Handler.
 * Called by the CPU when the card asserts its IRQ line.
 * 
 * @param regs Pointer to CPU registers structure.
 */
void rtl8139_handler(registers_t* regs) {
    /* Read the Interrupt Status Register (ISR) to see what happened */
    uint16_t status = inw(io_base + RTL8139_REG_ISR);
    
    /* Acknowledge the interrupts by writing the status back to the ISR.
     * In RTL8139, writing a '1' to a bit clears it. 
     */
    outw(io_base + RTL8139_REG_ISR, status);

    /* ROK: Receive OK */
    if (status & RTL8139_ISR_ROK) {
        terminal_writestring("RTL8139: Packet Received!\n");
        /**
         * FUTURE: To process packets, read from rx_buffer at the current CAPR offset.
         * Each packet has a header (Status + Length) followed by the Ethernet frame.
         */
    }

    /* TOK: Transmit OK */
    if (status & RTL8139_ISR_TOK) {
        terminal_writestring("RTL8139: Packet Transmitted!\n");
    }

    (void)regs;
}

/**
 * @brief Initialize the RTL8139 network card.
 * 
 * Initialization Sequence:
 * 1. Find device on PCI bus and enable Bus Mastering.
 * 2. Power on the card.
 * 3. Software Reset.
 * 4. Read MAC address.
 * 5. Setup DMA Receive Buffer.
 * 6. Set Interrupt Mask.
 * 7. Configure Receive modes.
 * 8. Enable RX/TX.
 */
void init_rtl8139() {
    /* Skip PCI-based detection for now - just announce RTL8139 is available */
    terminal_writestring("RTL8139: Driver stub loaded (PCI detection needed)\n");
    
    /* TODO: Implement proper PCI-based detection using pci_find_device() 
     * once the PCI bus scanning bug is fixed */
}

/**
 * @brief Send a raw packet.
 * 
 * @param data Pointer to the packet data in memory.
 * @param len Length of the packet in bytes.
 */
void rtl8139_send_packet(void* data, uint32_t len) {
    /* The RTL8139 has 4 transmit descriptors (0-3). Each has a 
     * Transmit Start Address (TSAD) and a Transmit Status Descriptor (TSD).
     */
    
    /* 1. Write the physical address of the packet data to TSAD */
    outw(io_base + RTL8139_REG_TSAD0 + (current_tx_desc * 4), (uint32_t)data);
    
    /** 
     * @brief 2. Write the length to TSD to trigger the transfer.
     * Bits 0-12: Size of the packet.
     * Bit 13: MUST be 0.
     * Bits 16-21: Transmit Threshold (0x3F = start transfer immediately).
     */
    outw(io_base + RTL8139_REG_TSD0 + (current_tx_desc * 4), len | 0x003F0000);

    /* Move to the next descriptor in a circular fashion */
    current_tx_desc++;
    if (current_tx_desc > 3) current_tx_desc = 0;
}
