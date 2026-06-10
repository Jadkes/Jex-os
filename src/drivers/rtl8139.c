/**
 * @file rtl8139.c
 * @brief RTL8139 Network Card Driver Implementation.
 *
 * Full initialization, interrupt-driven packet reception,
 * and DMA-based packet transmission for the Realtek 8139 NIC.
 *
 * The RTL8139 uses a 32KB ring buffer for reception via bus-mastering DMA.
 * Transmit uses 4 separate descriptor slots (round-robin).
 */

#include "rtl8139.h"
#include "ports.h"
#include "pmm.h"
#include "irq.h"
#include "terminal.h"
#include "serial.h"
#include "config.h"
#include "net.h"
#include <string.h>

/* Forward declarations for functions used from other compilation units */
extern void int_to_string(int n, char* str);

/* I/O Base Address and IRQ assigned by PCI */
static uint32_t io_base;
static uint8_t irq_num;

/* MAC Address read from the card's EEPROM */
static uint8_t mac_addr[6];

/* Receive Buffer: 32KB physically-contiguous ring buffer for DMA */
#define RX_BUF_SIZE (32 * 1024)
static uint8_t* rx_buffer;

/* Current transmit descriptor (0-3, round-robin) */
static uint32_t current_tx_desc = 0;

/* Track first-use of each TX descriptor — TOK spin-wait deadlocks on first call */
static uint8_t  tx_desc_used[4] = {0, 0, 0, 0};

/* Our read offset in the RX ring buffer (we don't trust CAPR readback) */
static uint16_t rx_offset = 0;

/* Driver state */
static int driver_initialized = 0;
static uint32_t rx_packet_count = 0;
static uint32_t tx_packet_count = 0;

/**
 * Print a hex byte to the terminal.
 */
static void print_hex_byte(uint8_t val)
{
    const char* hex = "0123456789ABCDEF";
    terminal_putchar(hex[(val >> 4) & 0xF]);
    terminal_putchar(hex[val & 0xF]);
}

/**
 * RTL8139 Interrupt Handler.
 *
 * Handles Receive OK (ROK) — reads packets from the ring buffer,
 * and Transmit OK (TOK) — confirms packet was sent.
 */
void rtl8139_handler(registers_t* regs)
{
    uint16_t status = inw(io_base + RTL8139_REG_ISR);

    /* Acknowledge by writing status back (writing 1 clears) */
    outw(io_base + RTL8139_REG_ISR, status);

    if (status & RTL8139_ISR_ROK) {
        uint16_t offset = rx_offset;

        while (1) {
            rx_packet_count++;
            uint16_t* header = (uint16_t*)(rx_buffer + offset);
            uint16_t rx_status = header[0];
            uint16_t rx_len = header[1];

            /* ROK bit (0x0001) in packet status means valid packet */
            if (!(rx_status & 0x0001))
                break;

            /* Sanity-check the length */
            if (rx_len > RX_BUF_SIZE || rx_len < 4)
                break;

            /* The Ethernet frame starts after the 4-byte header */
            uint8_t* frame = rx_buffer + offset + 4;
            net_process_packet(frame, rx_len);

            /* Advance past 4-byte header + payload, aligned to 4 bytes */
            offset += rx_len + 4;
            offset = (offset + 3) & ~3;
            if (offset >= RX_BUF_SIZE)
                offset -= RX_BUF_SIZE;

            /* Tell the card how far we've read */
            outw(io_base + RTL8139_REG_CAPR, offset);
        }

        /* Update our tracking of where we are in the ring */
        rx_offset = offset;
    }

    if (status & RTL8139_ISR_TOK) {
        log_serial("RTL8139: TX OK\n");
    }

    (void)regs;
}

/**
 * Initialize the RTL8139 network card.
 *
 * Sequence:
 *   PCI find → bus mastering → power-on → software reset →
 *   read MAC → allocate DMA RX buffer → configure RCR/IMR →
 *   register IRQ → enable RX/TX.
 */
void init_rtl8139()
{
    pci_device_t dev;
    int i;

    /* 1. Find the RTL8139 on the PCI bus */
    if (!pci_find_device(PCI_VENDOR_REALTEK, PCI_DEVICE_RTL8139, &dev)) {
        terminal_writestring("RTL8139: PCI device not found\n");
        log_serial("RTL8139: PCI device not found\n");
        return;
    }

    terminal_writestring("RTL8139: Found at ");
    print_hex_byte(dev.bus);
    terminal_putchar(':');
    print_hex_byte(dev.device);
    terminal_putchar('.');
    print_hex_byte(dev.function);
    terminal_writestring("\n");

    /* 2. Extract I/O base from BAR0 (mask off I/O indicator bits) */
    io_base = dev.bar0 & ~0x3;

    /* 3. Extract IRQ line */
    irq_num = dev.irq_line;

    log_serial("RTL8139: IO=");
    log_hex_serial(io_base);
    log_serial(" IRQ=");
    log_hex_serial(irq_num);
    log_serial("\n");

    /* 4. Enable PCI Bus Mastering (bit 2) and I/O Space (bit 0) */
    uint32_t pci_cmd = pci_config_read_dword(dev.bus, dev.device,
                                             dev.function, 0x04);
    pci_cmd |= (1 << 2) | (1 << 0);
    pci_config_write_dword(dev.bus, dev.device, dev.function, 0x04, pci_cmd);

    /* 5. Power on the chip via CONFIG1 (clear bit 0 = PMEn) */
    uint8_t config1 = inb(io_base + RTL8139_REG_CONFIG1);
    config1 &= ~(1 << 0);
    outb(io_base + RTL8139_REG_CONFIG1, config1);

    /* 6. Software reset (set CR_RST, poll until it clears) */
    outb(io_base + RTL8139_REG_CR, RTL8139_CR_RST);
    while (inb(io_base + RTL8139_REG_CR) & RTL8139_CR_RST)
        ;

    /* 7. Read MAC address from registers 0x00-0x05 */
    for (i = 0; i < 6; i++)
        mac_addr[i] = inb(io_base + RTL8139_REG_MAC0 + i);

    terminal_writestring("RTL8139: MAC ");
    for (i = 0; i < 6; i++) {
        print_hex_byte(mac_addr[i]);
        if (i < 5)
            terminal_putchar(':');
    }
    terminal_writestring("\n");

    /* 8. Allocate 32KB physically-contiguous RX DMA buffer */
    rx_buffer = (uint8_t*)pmm_alloc_blocks(RX_BUF_SIZE / PMM_BLOCK_SIZE);
    if (!rx_buffer) {
        terminal_writestring("RTL8139: RX buffer alloc failed\n");
        return;
    }
    memset(rx_buffer, 0, RX_BUF_SIZE);

    /* 9. Tell the card the RX buffer address */
    outl(io_base + RTL8139_REG_RBSTART, (uint32_t)rx_buffer);

    /* 10. Reset CAPR (Current Address of Packet Read) */
    outw(io_base + RTL8139_REG_CAPR, 0);
    rx_offset = 0;

    /* 11. Set interrupt mask: enable ROK and TOK */
    outw(io_base + RTL8139_REG_IMR, RTL8139_ISR_ROK | RTL8139_ISR_TOK);

    /*
     * 12. Configure receive mode:
     *     APM - Accept Physical Match (our MAC)
     *     AB  - Accept Broadcast
     *     AM  - Accept Multicast
     *     WRAP - Ring buffer wraps at end
     */
    outl(io_base + RTL8139_REG_RCR,
         RTL8139_RCR_APM | RTL8139_RCR_AB |
         RTL8139_RCR_AM  | RTL8139_RCR_WRAP);

    /* 13. Register interrupt handler. PIC maps IRQ0→0x20, so our irq_num is correct. */
    register_interrupt_handler(irq_num, rtl8139_handler);

    /* 14. Enable transmitter and receiver */
    outb(io_base + RTL8139_REG_CR, RTL8139_CR_TE | RTL8139_CR_RE);

    driver_initialized = 1;
    terminal_writestring("RTL8139: Initialized\n");
    log_serial("RTL8139: Initialized successfully\n");
}

/**
 * rtl8139_poll_rx - Poll the RX ring buffer for pending packets.
 *
 * Called from the main thread during wait loops (e.g. DNS resolution)
 * to process received packets when the interrupt may be delayed.
 * Disables interrupts during the ring read to prevent the ISR from
 * also draining the same packets.
 *
 * @return Number of packets processed.
 */
int rtl8139_poll_rx(void)
{
    /* Disable interrupts so the ISR doesn't also process the ring */
    __asm__ volatile("cli");

    int processed = 0;
    uint16_t offset = rx_offset;

    while (1) {
        uint16_t* header = (uint16_t*)(rx_buffer + offset);
        uint16_t rx_status = header[0];
        uint16_t rx_len    = header[1];

        /* ROK bit (0x0001) in packet status means valid packet */
        if (!(rx_status & 0x0001))
            break;

        if (rx_len > RX_BUF_SIZE || rx_len < 4)
            break;

        uint8_t* frame = rx_buffer + offset + 4;
        net_process_packet(frame, rx_len);

        /* Advance past 4-byte header + payload, aligned to 4 bytes */
        offset += rx_len + 4;
        offset = (offset + 3) & ~3;
        if (offset >= RX_BUF_SIZE)
            offset -= RX_BUF_SIZE;

        outw(io_base + RTL8139_REG_CAPR, offset);
        processed++;
    }

    rx_offset = offset;

    __asm__ volatile("sti");
    return processed;
}

/**
 * Send a raw Ethernet frame.
 *
 * The data buffer must be in DMA-accessible memory (identity-mapped
 * kernel space). Writes the buffer address to TSAD and triggers the
 * transfer via TSD.
 *
 * @param data Pointer to the packet data.
 * @param len  Length of the packet in bytes.
 */
void rtl8139_send_packet(void* data, uint32_t len)
{
    tx_packet_count++;

    uint16_t tsd_port = (uint16_t)(io_base + RTL8139_REG_TSD0 + current_tx_desc * 4);

    /* Wait for previous DMA on this descriptor (skip first use — no TOK yet) */
    if (tx_desc_used[current_tx_desc]) {
        while (!(inl(tsd_port) & 0x8000)) {
            __asm__ volatile("pause");
        }
    }
    tx_desc_used[current_tx_desc] = 1;

    /* Write the physical address of the data buffer to TSAD */
    outl(io_base + RTL8139_REG_TSAD0 + (current_tx_desc * 4),
         (uint32_t)data);

    /*
     * Trigger transmission: len in low 13 bits,
     * Early TX Threshold in bits 16-21 (0x3F = earliest start).
     */
    outl(io_base + RTL8139_REG_TSD0 + (current_tx_desc * 4),
         len | 0x003F0000);

    /* Advance to next descriptor (0 → 1 → 2 → 3 → 0) */
    current_tx_desc = (current_tx_desc + 1) & 3;
}

int rtl8139_is_initialized(void)
{
    return driver_initialized;
}

void rtl8139_get_mac(uint8_t* mac)
{
    for (int i = 0; i < 6; i++)
        mac[i] = mac_addr[i];
}

uint32_t rtl8139_get_io_base(void)
{
    return io_base;
}

uint8_t rtl8139_get_irq(void)
{
    return irq_num;
}

uint32_t rtl8139_get_rx_count(void)
{
    return rx_packet_count;
}

uint32_t rtl8139_get_tx_count(void)
{
    return tx_packet_count;
}

/**
 * Dump RTL8139 register state to the terminal for debugging.
 *
 * Reads live register values from the hardware and prints them alongside
 * driver-tracking state (MAC, packet counts, link status).
 */
void rtl8139_dump_regs(void)
{
    char buf[12];

    terminal_writestring("RTL8139 Registers:\n");

    /* MAC (IDR0-5) */
    terminal_writestring("  MAC: ");
    for (int i = 0; i < 6; i++) {
        print_hex_byte(mac_addr[i]);
        if (i < 5) terminal_putchar(':');
    }
    terminal_writestring("\n");

    /* Command Register */
    uint8_t cr = inb(io_base + RTL8139_REG_CR);
    terminal_writestring("  CR:  0x");
    print_hex_byte(cr);
    terminal_writestring("  (");
    if (cr & RTL8139_CR_TE) terminal_writestring("TE ");
    if (cr & RTL8139_CR_RE) terminal_writestring("RE ");
    if (cr & RTL8139_CR_RST) terminal_writestring("RST ");
    terminal_writestring(")\n");

    /* ISR */
    uint16_t isr = inw(io_base + RTL8139_REG_ISR);
    terminal_writestring("  ISR: 0x");
    print_hex_byte((isr >> 8) & 0xFF);
    print_hex_byte(isr & 0xFF);
    terminal_writestring("\n");

    /* IMR */
    uint16_t imr = inw(io_base + RTL8139_REG_IMR);
    terminal_writestring("  IMR: 0x");
    print_hex_byte((imr >> 8) & 0xFF);
    print_hex_byte(imr & 0xFF);
    terminal_writestring("\n");

    /* RX/TX counts */
    terminal_writestring("  TX:  ");
    int_to_string((int)tx_packet_count, buf);
    terminal_writestring(buf);
    terminal_writestring(" packets\n");
    terminal_writestring("  RX:  ");
    int_to_string((int)rx_packet_count, buf);
    terminal_writestring(buf);
    terminal_writestring(" packets\n");

    /* Link status from Media Status register at 0x58 */
    uint16_t media = inw(io_base + 0x58);
    terminal_writestring("  Link: ");
    terminal_writestring((media & 0x0001) ? "UP" : "DOWN");
    terminal_writestring("\n");
}
