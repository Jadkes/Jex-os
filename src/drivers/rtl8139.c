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
#include "pci.h"
#include "ports.h"
#include "pmm.h"
#include "irq.h"
#include "terminal.h"
#include "config.h"
#include <string.h>

/* I/O Base Address and IRQ assigned by the PCI bus */
static uint32_t io_base;
static uint8_t irq_num;

/* MAC Address read from the card's EEPROM */
static uint8_t mac_addr[6];

/* Receive Buffer: The card writes incoming packets here via DMA */
#define RX_BUF_SIZE (32 * 1024)
static uint8_t* rx_buffer;

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
    pci_device_t dev;
    /* Search for the card using IDs defined in config.h */
    if (!pci_find_device(PCI_VENDOR_REALTEK, PCI_DEVICE_RTL8139, &dev)) {
        terminal_writestring("RTL8139: Device not found!\n");
        return;
    }

    terminal_writestring("RTL8139: Found Realtek 8139 at PCI\n");

    /**
     * @brief Enable PCI Bus Mastering.
     * This is CRITICAL for DMA. Without this, the card cannot 
     * write received packets into our RAM.
     */
    uint32_t command = pci_config_read_dword(dev.bus, dev.device, dev.function, 0x04);
    command |= (1 << 2); // Set bit 2: Bus Master
    pci_config_write_dword(dev.bus, dev.device, dev.function, 0x04, command);

    /* BAR0 contains the I/O base address for the card's registers */
    io_base = dev.bar0 & ~0x1;
    irq_num = dev.irq_line;

    /* 1. Power on: Send 0x00 to Config1 to wake up the card from sleep mode */
    outb(io_base + RTL8139_REG_CONFIG1, 0x00);

    /* 2. Software Reset: Set the RST bit and wait for it to clear */
    outb(io_base + RTL8139_REG_CR, RTL8139_CR_RST);
    while ((inb(io_base + RTL8139_REG_CR) & RTL8139_CR_RST) != 0);

    /* 3. Read MAC Address: The first 6 bytes of I/O space are the ID registers */
    for (int i = 0; i < 6; i++) {
        mac_addr[i] = inb(io_base + RTL8139_REG_MAC0 + i);
    }

    /** 
     * @brief 4. Setup Receive Buffer.
     * We use a 32KB ring buffer. The card also needs 16 bytes of padding
     * to handle packet wrap-around without software intervention.
     * pmm_alloc_blocks(9) gives us 36KB (9 * 4KB).
     */
    rx_buffer = (uint8_t*)pmm_alloc_blocks(9);
    memset(rx_buffer, 0, 9 * 4096);
    /* Tell the card the physical address of our buffer */
    outw(io_base + RTL8139_REG_RBSTART, (uint32_t)rx_buffer);

    /* 5. Set Interrupt Mask: Enable ROK (Receive OK) and TOK (Transmit OK) interrupts */
    outw(io_base + RTL8139_REG_IMR, RTL8139_ISR_ROK | RTL8139_ISR_TOK);

    /**
     * @brief 6. Configure Receive (RCR).
     * AAP: Accept All Packets (Promiscuous)
     * APM: Accept Physical Match (Our MAC)
     * AM: Accept Multicast
     * AB: Accept Broadcast
     * WRAP: If bit 7 is set, the card will wrap packets that exceed the buffer end
     *       using the extra 1.5KB of padding space.
     * Buffer Size: 32KB is indicated by 10 in bits 11-12.
     */
    uint32_t rcr = RTL8139_RCR_AAP | RTL8139_RCR_APM | RTL8139_RCR_AM | 
                   RTL8139_RCR_AB  | RTL8139_RCR_WRAP | (2 << 11);
    outw(io_base + RTL8139_REG_RCR, rcr);

    /* 7. Enable RX and TX: Set RE (Receive Enable) and TE (Transmit Enable) bits */
    outb(io_base + RTL8139_REG_CR, RTL8139_CR_RE | RTL8139_CR_TE);

    /* 8. Register the C handler for the assigned IRQ */
    register_interrupt_handler(irq_num, rtl8139_handler);

    terminal_writestring("RTL8139: Driver successfully initialized.\n");
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
