/**
 * @file net.c
 * @brief Network stack — ARP, IP, ICMP, UDP, DNS.
 *
 * FIXME: reply_buf double-DMA race in ISR batch processing.
 * DONE:   strict-aliasing violations in DNS RX path (read_be16 helper).
 * DONE:   udp_checksum 1512-byte buffer moved off the IRQ stack.
 * DONE:   DNS handler minimum-valid-length check (was < 2, now < 12).
 * DONE:   ARP timeout increased for QEMU slirp compatibility.
 */

#define pr_fmt(fmt) "[NET] " fmt
#include "kernel/printk.h"
#include "net.h"
#include "rtl8139.h"
#include "terminal.h"
#include "serial.h"
#include "timer.h"
#include "kheap.h"
#include "panic.h"
#include "kernel/backtrace.h"
#include <string.h>

/* Forward: int_to_string is defined in shell.c */
extern void int_to_string(int n, char* str);

/* TX Buffers */

static uint8_t send_buf[PACKET_BUF_SIZE];       /* shell-initiated transmits */
static uint8_t reply_buf[PACKET_BUF_SIZE];      /* IRQ reply path */
/* FIXME: reply_buf double-DMA race when ISR processes >1 packet needing a reply */

#define ARP_CACHE_SIZE 4

typedef struct {
    uint32_t ip;
    uint8_t  mac[6];
    volatile int valid;
} arp_entry_t;

static arp_entry_t arp_cache[ARP_CACHE_SIZE];

/* State */

static volatile int ping_responses = 0;
static uint16_t ip_id              = 0;
static uint16_t icmp_seq           = 0;

/* Verbose logging */

static volatile int verbose_net_flags = NETLOG_OFF;

static void netlog_write(const char* prefix, const char* msg)
{
    pr_info("%s: %s\n", prefix, msg);
}

void netlog_set_flags(int flags)
{
    verbose_net_flags = flags;
    pr_info("verbose logging %s\n", flags ? "enabled" : "disabled");
}

/* Checksum & helpers */

/*
 * checksum - 16-bit one's complement checksum over len bytes.
 *
 * WHY: Standard Internet checksum used by IP, ICMP, UDP (with pseudo-header).
 *      Accumulates 16-bit words, folds carry bits, and returns the complement.
 *
 * @param buf  Data viewed as 16-bit words.
 * @param len  Length in bytes (odd length folds the trailing byte).
 * @return     The one's complement checksum.
 */
uint16_t checksum(uint16_t* buf, uint32_t len)
{
    uint32_t sum = 0;

    for (uint32_t i = 0; i < len / 2; i++)
        sum += buf[i];

    /* Fold in a trailing odd byte */
    if (len & 1)
        sum += ((uint8_t*)buf)[len - 1];

    /* Fold 32-bit sum into 16 bits (carry may iterate multiple times) */
    while (sum >> 16)
        sum = (sum & 0xFFFF) + (sum >> 16);

    return ~(uint16_t)sum & 0xFFFF;
}

/*
 * read_be16 - Read a 16-bit big-endian value via memcpy (strict-aliasing safe).
 *
 * WHY: Casting uint8_t* to uint16_t* violates C99 §6.5 effective-type rules.
 *      memcpy is the standard-compliant way to type-pun; the compiler
 *      optimises the call to a single load on any half-decent arch.
 *
 * @param p  Pointer (need not be aligned).
 * @return    Big-endian value converted to host byte order.
 */
static inline uint16_t read_be16(const uint8_t* p)
{
    uint16_t v;
    memcpy(&v, p, sizeof(v));
    return ntohs(v);
}

/*
 * arp_find - Search the cache for a given IP.
 * @param ip  Target IP (network byte order).
 * @return    Index into arp_cache[], or -1 if not found.
 */
static int arp_find(uint32_t ip)
{
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (arp_cache[i].valid && arp_cache[i].ip == ip)
            return i;
        /* Compiler barrier: re-read arp_cache — ISR may have updated it */
        __asm__ volatile("" ::: "memory");
    }
    return -1;
}

/*
 * arp_add - Insert or update a MAC<->IP mapping.
 *
 * If the IP already exists, update its MAC.  Otherwise fill the first
 * empty slot, or evict the oldest entry (FIFO) if full.
 *
 * @param ip   Target IP (network byte order).
 * @param mac  6-byte MAC address to associate.
 */
static void arp_add(uint32_t ip, const uint8_t* mac)
{
    int idx = arp_find(ip);
    if (idx >= 0) {
        memcpy(arp_cache[idx].mac, mac, 6);
        return;
    }
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (!arp_cache[i].valid) {
            arp_cache[i].ip = ip;
            memcpy(arp_cache[i].mac, mac, 6);
            /* Barrier: ip/mac written before valid=1 visible to main thread */
            __asm__ volatile("" ::: "memory");
            arp_cache[i].valid = 1;
            return;
        }
    }
    /* Evict oldest (shift left, insert at end) */
    for (int i = 1; i < ARP_CACHE_SIZE; i++)
        arp_cache[i - 1] = arp_cache[i];
    arp_cache[ARP_CACHE_SIZE - 1].ip = ip;
    memcpy(arp_cache[ARP_CACHE_SIZE - 1].mac, mac, 6);
    /* Barrier: eviction writes done before valid=1 visible */
    __asm__ volatile("" ::: "memory");
    arp_cache[ARP_CACHE_SIZE - 1].valid = 1;
}

/*
 * arp_lookup - Public: find cached MAC for an IP.
 *
 * @param ip  Target IP (network byte order).
 * @return    Index into arp_cache[], or -1 if not cached.
 */
int arp_lookup(uint32_t ip)
{
    return arp_find(ip);
}

/*
 * arp_dump - Print the contents of the ARP cache to terminal and serial.
 *
 * Walks all ARP_CACHE_SIZE slots and prints IP, MAC and validity status
 * for each one.  Used by the shell's "arp" command.
 */
void arp_dump(void)
{
    char buf[16];
    int count = 0;

    /* Disable interrupts while walking the cache (ISR may modify it) */
    __asm__ volatile("cli");

    for (int i = 0; i < ARP_CACHE_SIZE; i++)
        if (arp_cache[i].valid) count++;

    terminal_writestring("ARP Cache (");
    int_to_string(count, buf);
    terminal_writestring(buf);
    terminal_writestring("/");
    int_to_string(ARP_CACHE_SIZE, buf);
    terminal_writestring(buf);
    terminal_writestring(" entries):\n");

    const char* hex = "0123456789ABCDEF";

    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (arp_cache[i].valid) {
            /* IP stored in network order; byte-at-a-time gives dotted-decimal on LE x86 */
            uint8_t* ip = (uint8_t*)&arp_cache[i].ip;
            int_to_string(ip[0], buf); terminal_writestring(buf); terminal_putchar('.');
            int_to_string(ip[1], buf); terminal_writestring(buf); terminal_putchar('.');
            int_to_string(ip[2], buf); terminal_writestring(buf); terminal_putchar('.');
            int_to_string(ip[3], buf); terminal_writestring(buf);

            terminal_writestring("  ->  ");

            /* Print MAC */
            for (int j = 0; j < 6; j++) {
                terminal_putchar(hex[(arp_cache[i].mac[j] >> 4) & 0xF]);
                terminal_putchar(hex[arp_cache[i].mac[j] & 0xF]);
                if (j < 5) terminal_putchar(':');
            }
            terminal_writestring("  (valid)\n");

            pr_debug("cache entry: IP=0x%x\n", arp_cache[i].ip);
        } else {
            terminal_writestring("  -- slot ");
            int_to_string(i, buf);
            terminal_writestring(buf);
            terminal_writestring(" empty --\n");
        }
    }

    /* Re-enable interrupts now that the cache walk is done */
    __asm__ volatile("sti");
}

/* Routing */

/*
 * is_local_ip - Check if an IP is on the local subnet.
 *
 * QEMU slirp uses the 10.0.2.0/24 range by default.  Anything outside
 * that must be reached via the gateway (10.0.2.2), which provides NAT.
 *
 * @param ip  IP in network byte order.
 * @return  1 if local, 0 if remote.
 */
int is_local_ip(uint32_t ip)
{
    /* Our /24 subnet: any 10.0.2.x address is local */
    return (ip & htonl(0xFFFFFF00)) == (OUR_IP & htonl(0xFFFFFF00));
}

/*
 * resolve_mac - Get the MAC address to reach a destination IP.
 *
 * For local IPs: looks up the ARP cache for the destination directly.
 * For remote IPs: looks up the ARP cache for the gateway instead.
 *
 * @param ip      Destination IP (network byte order).
 * @param mac_out 6-byte buffer for the resolved MAC.
 * @return        0 on success with mac_out filled, -1 if not cached.
 */
int resolve_mac(uint32_t ip, uint8_t* mac_out)
{
    uint32_t resolve_ip;

    if (is_local_ip(ip)) {
        resolve_ip = ip;
    } else {
        resolve_ip = GATEWAY_IP;
    }

    int idx = arp_find(resolve_ip);
    if (idx < 0)
        return -1;

    memcpy(mac_out, arp_cache[idx].mac, ETH_ALEN);
    return 0;
}

/*
 * net_arp_resolve - Broadcast an ARP request for the right target.
 *
 * For local destinations, ARPs for the IP directly.
 * For remote destinations, ARPs for the gateway (which will forward).
 * Skips if the target MAC is already cached.
 *
 * @param ip  Destination IP (network byte order).
 */
void net_arp_resolve(uint32_t ip)
{
    uint32_t arp_target;

    if (is_local_ip(ip)) {
        arp_target = ip;
        pr_debug("local IP, ARPing for target\n");
    } else {
        arp_target = GATEWAY_IP;
        pr_debug("remote IP, ARPing for gateway\n");
    }

    /* If already cached, skip */
    if (arp_find(arp_target) >= 0) {
        pr_debug("target already in ARP cache\n");
        return;
    }

    uint8_t our_mac[6];
    rtl8139_get_mac(our_mac);
    uint8_t broadcast[ETH_ALEN] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
    uint8_t* buf = send_buf;
    uint8_t* p   = buf;

    p = build_ether_header(p, broadcast, our_mac, ETHERTYPE_ARP);

    arp_packet_t* ap = (arp_packet_t*)p;
    ap->htype = htons(ARP_HTYPE_ETHER);
    ap->ptype = htons(ARP_PROTO_IP);
    ap->hlen  = ETH_ALEN;
    ap->plen  = 4;
    ap->oper  = htons(ARP_OP_REQUEST);
    memcpy(ap->sha, our_mac, ETH_ALEN);
    ap->spa   = OUR_IP;
    memset(ap->tha, 0, ETH_ALEN);
    ap->tpa   = arp_target;

    net_send_ether(buf, sizeof(eth_header_t) + sizeof(arp_packet_t));
    pr_debug("ARP request sent\n");
}

/*
 * route_print - Display the routing table.
 */
void route_print(void)
{
    char buf[16];
    uint32_t gw = GATEWAY_IP;
    uint8_t* gwb = (uint8_t*)&gw;

    terminal_writestring("Routing table:\n");
    pr_debug("displaying routing table\n");

    /* Local network */
    terminal_writestring("  Destination     Netmask          Gateway          Iface\n");
    terminal_writestring("  10.0.2.0        255.255.255.0    link#1           rtl8139\n");

    /* Default route */
    terminal_writestring("  default         -                ");
    int_to_string(gwb[0], buf); terminal_writestring(buf); terminal_putchar('.');
    int_to_string(gwb[1], buf); terminal_writestring(buf); terminal_putchar('.');
    int_to_string(gwb[2], buf); terminal_writestring(buf); terminal_putchar('.');
    int_to_string(gwb[3], buf); terminal_writestring(buf);
    terminal_writestring("         rtl8139\n");

    pr_debug("routing table shown\n");
}

/*
 * build_ether_header - Write an Ethernet header and advance past it.
 *
 * @param buf   Destination buffer.
 * @param dest  Destination MAC (6 bytes).
 * @param src   Source MAC (6 bytes).
 * @param type  EtherType (e.g. ETHERTYPE_IP).
 * @return      buf + sizeof(eth_header_t).
 */
uint8_t* build_ether_header(uint8_t* buf, const uint8_t* dest,
                            const uint8_t* src, uint16_t type)
{
    eth_header_t* eth = (eth_header_t*)buf;
    memcpy(eth->dest, dest, ETH_ALEN);
    memcpy(eth->src,  src,  ETH_ALEN);
    eth->type = htons(type);
    return buf + sizeof(eth_header_t);
}

/*
 * build_ip_header - Write an IP header, compute checksum, advance past it.
 *
 * Fills a standard 20-byte IP header (no options) with TTL=64 and an
 * auto-incrementing ID.  The caller should write payload data after the
 * returned pointer, then call net_send_ether() with total_len =
 * IP_HEADER_LEN + data_len.
 *
 * @param buf       Destination buffer.
 * @param protocol  L4 protocol (IP_PROTO_ICMP, IP_PROTO_UDP, ...).
 * @param src_ip    Source IP (network order).
 * @param dest_ip   Destination IP (network order).
 * @param data_len  Length of payload after the IP header.
 * @return          buf + IP_HEADER_LEN.
 */
uint8_t* build_ip_header(uint8_t* buf, uint8_t protocol,
                         uint32_t src_ip, uint32_t dest_ip,
                         uint16_t data_len)
{
    ip_header_t* ip = (ip_header_t*)buf;

    ip->ver_ihl    = 0x45;                     /* IPv4, 5×4 = 20 bytes */
    ip->dscp_ecn   = 0;
    ip->total_len  = htons(IP_HEADER_LEN + data_len);
    ip->id         = htons(ip_id++);
    ip->flags_frag = htons(0x4000);            /* Don't Fragment flag */
    ip->ttl        = 64;
    ip->protocol   = protocol;
    ip->checksum   = 0;
    ip->src_ip     = src_ip;
    ip->dest_ip    = dest_ip;
    ip->checksum   = checksum((uint16_t*)ip, IP_HEADER_LEN);

    return buf + IP_HEADER_LEN;
}

/*
 * net_send_ether - Send a raw Ethernet frame via the NIC driver.
 *
 * Thin wrapper insulating upper layers from the rtl8139 dependency.
 */
void net_send_ether(void* data, uint32_t len)
{
    if (verbose_net_flags)
        pr_debug("TX: len=0x%x\n", len);

    rtl8139_send_packet(data, len);
}

/*
 * handle_arp - Process an incoming ARP packet.
 *
 * Learns the sender's MAC address.  If the packet is a request for
 * OUR_IP, sends an ARP reply via the IRQ reply buffer.
 *
 * @param data  Pointer to the ARP payload (after Ethernet header).
 * @param len   Length of the payload.
 */
static void handle_arp(uint8_t* data, uint32_t len)
{
    if (len < sizeof(arp_packet_t))
        return;

    if (verbose_net_flags & NETLOG_ARP) {
        netlog_write("ARP", "received packet");
    }

    arp_packet_t* ap = (arp_packet_t*)data;
    uint16_t oper = ntohs(ap->oper);
    pr_debug("received oper=0x%x\n", oper);

    /* Validate hardware / protocol types */
    if (ntohs(ap->htype) != ARP_HTYPE_ETHER ||
        ntohs(ap->ptype) != ARP_PROTO_IP)
        return;
    if (ap->hlen != ETH_ALEN || ap->plen != 4)
        return;

    /* Cache the sender's address */
    arp_add(ap->spa, ap->sha);

    /* Reply only to requests directed at us */
    if (oper != ARP_OP_REQUEST || ap->tpa != OUR_IP)
        return;

    uint8_t our_mac[6];
    rtl8139_get_mac(our_mac);

    /* Build the reply in the IRQ buffer */
    uint8_t* buf = reply_buf;
    uint8_t* p   = buf;

    p = build_ether_header(p, ap->sha, our_mac, ETHERTYPE_ARP);

    arp_packet_t* reply = (arp_packet_t*)p;
    reply->htype = htons(ARP_HTYPE_ETHER);
    reply->ptype = htons(ARP_PROTO_IP);
    reply->hlen  = ETH_ALEN;
    reply->plen  = 4;
    reply->oper  = htons(ARP_OP_REPLY);
    memcpy(reply->sha, our_mac, ETH_ALEN);
    reply->spa   = OUR_IP;
    memcpy(reply->tha, ap->sha, ETH_ALEN);
    reply->tpa   = ap->spa;

    net_send_ether(buf, sizeof(eth_header_t) + sizeof(arp_packet_t));
    pr_debug("ARP reply sent\n");
}


#define UDP_MAX_HANDLERS 8

typedef struct {
    uint16_t        port;           /* Host byte order               */
    udp_callback_t  handler;
    void*           userdata;
    int             active;
} udp_handler_entry_t;

static udp_handler_entry_t udp_handlers[UDP_MAX_HANDLERS];

/*
 * udp_checksum - Compute UDP checksum with the pseudo-header.
 *
 * WHY: RFC 768 requires a 12-byte pseudo-header (src IP, dest IP,
 *      protocol, UDP length) to be prepended to the UDP datagram
 *      for checksum calculation.  We build it on the stack, compute
 *      the combined checksum, and discard the pseudo-header.
 *
 * DONE: Buffer moved from stack to static; was 1512 bytes on stack in ISR.
 *       Single-core design means no re-entrancy concern.
 *
 * @param src_ip    Source IP (network order).
 * @param dest_ip   Destination IP (network order).
 * @param udp_hdr   Pointer to the UDP header.
 * @param udp_len   Length of UDP header + data.
 * @return          The 16-bit checksum (0 if result is 0 → send as 0xFFFF).
 */
static uint16_t udp_checksum(uint32_t src_ip, uint32_t dest_ip,
                              const uint16_t* udp_hdr, uint32_t udp_len)
{
    udp_pseudo_t pseudo;
    pseudo.src_ip   = src_ip;
    pseudo.dest_ip  = dest_ip;
    pseudo.zero     = 0;
    pseudo.protocol = IP_PROTO_UDP;
    pseudo.udp_len  = htons(udp_len);

    /*
     * We need to checksum pseudo-header + UDP datagram as one contiguous
     * block.  Static buffer avoids a 1500-byte stack allocation.
     *
     * PROTECTION: net_send_udp (shell context) wraps the call with cli/sti
     * so the ISR cannot clobber this buffer.  handle_udp (ISR context)
     * calls this with interrupts already disabled by the CPU's interrupt
     * gate, so cli is already in effect.  Either way, the buffer is safe.
     */
    static uint8_t buf[sizeof(udp_pseudo_t) + 1500];
    uint32_t total = sizeof(udp_pseudo_t) + udp_len;
    if (total > sizeof(buf))
        return 0;       /* Too large — skip checksum */

    memcpy(buf, &pseudo, sizeof(udp_pseudo_t));
    memcpy(buf + sizeof(udp_pseudo_t), udp_hdr, udp_len);

    uint16_t sum = checksum((uint16_t*)buf, total);
    /* RFC 768: 0 means "no checksum" — if we computed 0, send 0xFFFF */
    return sum ? sum : 0xFFFF;
}

/*
 * handle_udp - Process an incoming UDP datagram.
 *
 * Validates length, optionally verifies the checksum, looks up the
 * destination port in the registered-handler table, and dispatches.
 *
 * @param data    Pointer to the IP header (whole datagram).
 * @param len     Total length from IP header onward.
 * @param ip_hdr  Length of the IP header in bytes.
 */
static void handle_udp(uint8_t* data, uint32_t len, uint32_t ip_hdr)
{
    uint32_t ip_tot = ntohs(((ip_header_t*)data)->total_len);

    /* Must at least fit the UDP header (after IP header) */
    if (len < ip_hdr + sizeof(udp_header_t))
        return;
    if (ip_tot < ip_hdr + sizeof(udp_header_t))
        return;

    udp_header_t* udp  = (udp_header_t*)(data + ip_hdr);
    uint16_t udp_len   = ntohs(udp->length);
    uint32_t remaining = ip_tot - ip_hdr;

    /* UDP length must match (or exceed; RFC 768 says discard if less) */
    if (udp_len < sizeof(udp_header_t) || udp_len > remaining)
        return;

    uint16_t dest_port = ntohs(udp->dest_port);

    if (dest_port == DNS_CLIENT_PORT)
        pr_debug("received on DNS port len=%u\n", udp_len);

    /* Verify checksum if non-zero (zero means sender omitted it) */
    if (udp->checksum != 0) {
        uint16_t saved_csum = udp->checksum;
        udp->checksum = 0;
        uint16_t computed = udp_checksum(
            ((ip_header_t*)data)->src_ip,
            ((ip_header_t*)data)->dest_ip,
            (const uint16_t*)udp, udp_len);
        udp->checksum = saved_csum;
        if (saved_csum != computed) {
            if (dest_port == DNS_CLIENT_PORT)
                pr_debug("DNS csum mismatch (saved=0x%x computed=0x%x) DROPPED\n",
                         saved_csum, computed);
            return;
        }
    }

    /* Look up handler by destination port.
     * Disable interrupts so the table doesn't change under us. */
    __asm__ volatile("cli");
    for (int i = 0; i < UDP_MAX_HANDLERS; i++) {
        if (udp_handlers[i].active && udp_handlers[i].port == dest_port) {
            if (dest_port == DNS_CLIENT_PORT)
                pr_debug("DNS handler found\n");
            /* Snapshot handler + userdata before re-enabling interrupts,
             * since the handler may itself register/unregister. */
            udp_callback_t h = udp_handlers[i].handler;
            void* udata      = udp_handlers[i].userdata;
            __asm__ volatile("sti");
            h(
                ((ip_header_t*)data)->src_ip,
                udp->src_port,
                (uint8_t*)(udp + 1),        /* Payload starts after UDP hdr */
                udp_len - sizeof(udp_header_t),
                udata);
            return;
        }
    }
    __asm__ volatile("sti");
}

/*
 * net_udp_register - Register a handler for an incoming UDP port.
 *
 * @param port     Port in host byte order.
 * @param handler  Callback (called from IRQ context — keep it short).
 * @param userdata Opaque pointer for the callback.
 * @return         0 on success, -1 if table full.
 */
int net_udp_register(uint16_t port, udp_callback_t handler, void* userdata)
{
    __asm__ volatile("cli");
    /* Overwrite existing entry if the port is already registered */
    for (int i = 0; i < UDP_MAX_HANDLERS; i++) {
        if (udp_handlers[i].active && udp_handlers[i].port == port) {
            udp_handlers[i].handler  = handler;
            udp_handlers[i].userdata = userdata;
            __asm__ volatile("sti");
            return 0;
        }
    }

    /* Find a free slot — set handler/userdata BEFORE marking active */
    for (int i = 0; i < UDP_MAX_HANDLERS; i++) {
        if (!udp_handlers[i].active) {
            udp_handlers[i].port     = port;
            udp_handlers[i].handler  = handler;
            udp_handlers[i].userdata = userdata;
            /* Memory barrier: ensure handler/userdata are visible
             * before the ISR can see active=1. */
            __asm__ volatile("" ::: "memory");
            udp_handlers[i].active   = 1;
            __asm__ volatile("sti");
            return 0;
        }
    }
    __asm__ volatile("sti");
    return -1;      /* Table full */
}

/*
 * net_udp_unregister - Remove a UDP handler.
 *
 * @param port  Port in host byte order.
 */
void net_udp_unregister(uint16_t port)
{
    __asm__ volatile("cli");
    for (int i = 0; i < UDP_MAX_HANDLERS; i++) {
        if (udp_handlers[i].active && udp_handlers[i].port == port) {
            udp_handlers[i].active = 0;
            __asm__ volatile("sti");
            return;
        }
    }
    __asm__ volatile("sti");
}

/*
 * net_send_udp - Build and send a UDP datagram.
 *
 * Resolves the destination via ARP.  If the IP is not cached, returns
 * -1 (caller should trigger ARP resolution and retry).
 *
 * Computes the UDP checksum with the RFC 768 pseudo-header.
 *
 * @param dest_ip   Destination IP (network byte order).
 * @param dest_port Destination port (host byte order).
 * @param src_port  Source port (host byte order).
 * @param data      Payload data.
 * @param len       Payload length in bytes.
 * @return          0 on success, -1 if ARP not resolved.
 */
int net_send_udp(uint32_t dest_ip, uint16_t dest_port, uint16_t src_port,
                 const uint8_t* data, uint32_t len)
{
    uint8_t our_mac[6];
    rtl8139_get_mac(our_mac);

    /* Resolve MAC (local via ARP, remote via gateway) */
    uint8_t dest_mac[6];
    if (resolve_mac(dest_ip, dest_mac) < 0)
        return -1;

    uint32_t udp_len     = sizeof(udp_header_t) + len;
    uint32_t ip_data_len = udp_len;     /* What IP considers "payload" */

    /* Build the packet in send_buf */
    uint8_t* buf = send_buf;
    uint8_t* p   = buf;

    p = build_ether_header(p, dest_mac, our_mac, ETHERTYPE_IP);

    /* IP header (UDP is the payload) */
    p = build_ip_header(p, IP_PROTO_UDP, OUR_IP, dest_ip, ip_data_len);

    /* UDP header */
    udp_header_t* udp = (udp_header_t*)p;
    udp->src_port  = htons(src_port);
    udp->dest_port = htons(dest_port);
    udp->length    = htons(udp_len);
    udp->checksum  = 0;                         /* Temp: zero before calc */
    p += sizeof(udp_header_t);

    /* Payload */
    memcpy(p, data, len);
    p += len;

    /* Compute UDP checksum with pseudo-header (RFC 768).
     * The checksum field was zeroed above before payload copy.
     * RFC 768: if computed checksum is 0, send 0xFFFF (0 means "no checksum").
     *
     * WHY cli/sti: udp_checksum uses a static scratch buffer shared with
     * handle_udp (ISR context).  Disabling interrupts prevents the ISR
     * from clobbering the buffer mid-computation.  The window is < 100 us
     * so the risk of missed Rx is negligible. */
    {
        __asm__ volatile("cli");
        uint16_t csum = udp_checksum(OUR_IP, dest_ip, (const uint16_t*)udp, udp_len);
        __asm__ volatile("sti");
        udp->checksum = (csum == 0) ? 0xFFFF : csum;
    }

    uint32_t total = sizeof(eth_header_t) + IP_HEADER_LEN + ip_data_len;
    net_send_ether(buf, total);
    return 0;
}

/*
 * handle_icmp - Process an ICMP message inside an IP datagram.
 *
 * On Echo Request addressed to us: swap src/dest in the IP header,
 * flip the ICMP type to Echo Reply, recompute checksums, and send
 * the reply via the IRQ buffer.
 *
 * On Echo Reply addressed to us: increment ping_responses so the
 * shell's ping_command can detect the reply.
 *
 * @param data    Pointer to the IP header (whole datagram).
 * @param len     Total length from IP header onward.
 * @param src_mac Source MAC from the Ethernet header (for the reply).
 */
static void handle_icmp(uint8_t* data, uint32_t len, const uint8_t* src_mac)
{
    ip_header_t* ip  = (ip_header_t*)data;
    uint32_t ip_hdr  = (ip->ver_ihl & 0x0F) * 4;
    uint32_t ip_tot  = ntohs(ip->total_len);

    if (len < ip_hdr + sizeof(icmp_header_t))
        return;
    if (ip_tot < ip_hdr + sizeof(icmp_header_t))
        return;

    if (verbose_net_flags & NETLOG_ICMP) {
        netlog_write("ICMP", "packet received");
    }

    icmp_header_t* icmp = (icmp_header_t*)(data + ip_hdr);
    uint32_t icmp_len   = ip_tot - ip_hdr;

    /* ---- Echo Request: send reply ---- */
    if (icmp->type == ICMP_ECHO_REQUEST && ip->dest_ip == OUR_IP) {
        uint8_t our_mac[6];
        rtl8139_get_mac(our_mac);

        uint32_t total = sizeof(eth_header_t) + ip_tot;
        if (total > PACKET_BUF_SIZE)
            return;

        uint8_t* buf = reply_buf;
        uint8_t* p   = buf;

        p = build_ether_header(p, src_mac, our_mac, ETHERTYPE_IP);

        /* Copy the incoming IP datagram (including payload) */
        memcpy(p, data, ip_tot);

        /* Fix IP header: swap src↔dest, recalculate checksum */
        ip_header_t* rip = (ip_header_t*)p;
        rip->dest_ip  = ip->src_ip;
        rip->src_ip   = OUR_IP;
        rip->checksum = 0;
        rip->checksum = checksum((uint16_t*)rip, ip_hdr);

        /* Fix ICMP: Echo Request → Echo Reply */
        icmp_header_t* ricmp = (icmp_header_t*)(p + ip_hdr);
        ricmp->type    = ICMP_ECHO_REPLY;
        ricmp->code    = 0;
        ricmp->checksum = 0;
        ricmp->checksum = checksum((uint16_t*)ricmp, icmp_len);

        net_send_ether(buf, total);
        pr_debug("echo reply sent\n");
    }

    /* ---- Echo Reply: count it ---- */
    if (icmp->type == ICMP_ECHO_REPLY && ip->dest_ip == OUR_IP) {
        if (verbose_net_flags & NETLOG_ICMP) {
            netlog_write("ICMP", "echo reply received");
        }
        ping_responses++;
        pr_debug("echo reply received\n");
    }
}

/*
 * handle_ip - Process an incoming IP datagram.
 *
 * Validates length and header checksum, learns the sender's MAC via
 * ARP cache, then dispatches to the appropriate L4 handler based on
 * the protocol field.
 *
 * @param data    Pointer to the IP header (after Ethernet header).
 * @param len     Remaining packet length from IP header onward.
 * @param src_mac Source MAC from the Ethernet header.
 */
static void handle_ip(uint8_t* data, uint32_t len, const uint8_t* src_mac)
{
    /* Must at least fit the IP header */
    if (len < IP_HEADER_LEN) {
        pr_debug("too short\n");
        return;
    }

    ip_header_t* ip   = (ip_header_t*)data;
    uint32_t     ip_hdr = (ip->ver_ihl & 0x0F) * 4;

    if (len < ip_hdr) {
        pr_debug("header longer than data\n");
        return;
    }

    /* Verify header checksum: checksum() over the full header
     * (including the stored checksum field) returns 0 if valid. */
    if (checksum((uint16_t*)ip, ip_hdr) != 0) {
        if (verbose_net_flags & NETLOG_IP)
            netlog_write("IP", "bad checksum");
        pr_debug("bad IP checksum\n");
        return;
    }

    /* Learn the sender's MAC for future replies */
    arp_add(ip->src_ip, src_mac);

    /* Only accept packets addressed to us */
    if (ip->dest_ip != OUR_IP)
        return;

    /* Dispatch to the appropriate L4 protocol handler */
    switch (ip->protocol) {
    case IP_PROTO_ICMP:
        handle_icmp(data, len, src_mac);
        break;
    case IP_PROTO_UDP:
        handle_udp(data, len, ip_hdr);
        break;
    case IP_PROTO_TCP:
        handle_tcp(data, len, ip_hdr, src_mac);
        break;
    }
}

/* Forward declaration for static capture function used here */
static void tcpdump_capture(uint8_t* data, uint32_t len, uint16_t ethertype);

/*
 * net_process_packet - Demux an incoming Ethernet frame.
 *
 * Called from the RTL8139 interrupt handler for every received packet.
 * Strips the Ethernet header and dispatches by EtherType.
 *
 * @param data  Pointer to the raw Ethernet frame.
 * @param len   Total frame length including Ethernet header.
 */
void net_process_packet(uint8_t* data, uint32_t len)
{
    if (len < sizeof(eth_header_t))
        return;

    eth_header_t* eth  = (eth_header_t*)data;
    uint16_t      type = ntohs(eth->type);
    uint8_t*      payload = data + sizeof(eth_header_t);
    uint32_t      plen    = len - sizeof(eth_header_t);

    /* tcpdump capture hook */
    tcpdump_capture(data, len, type);

    pr_debug("rx type=0x%x len=0x%x\n", type, len);

    switch (type) {
    case ETHERTYPE_ARP:
        handle_arp(payload, plen);
        break;
    case ETHERTYPE_IP:
        handle_ip(payload, plen, eth->src);
        break;
    default:
        pr_debug("unknown ethertype, dropping\n");
        break;
    }
}

/*
 * net_ping - Send an ICMP Echo Request to a given IP.
 *
 * If the destination is not in the ARP cache, sends an ARP request
 * instead and returns -1 (caller should retry after ARP resolution).
 * Otherwise builds the full Ethernet+IP+ICMP packet and sends it.
 *
 * @param dest_ip  Target IP (network byte order).
 * @return         0 on success, -1 if ARP resolution was needed.
 */
int net_ping(uint32_t dest_ip)
{
    uint8_t our_mac[6];
    rtl8139_get_mac(our_mac);

    /* ---- Resolve destination MAC (local or via gateway) ---- */
    uint8_t dest_mac[6];
    if (resolve_mac(dest_ip, dest_mac) < 0) {
        /* MAC not cached — broadcast ARP request for the right target */
        net_arp_resolve(dest_ip);
        return -1;
    }

    /* ---- MAC resolved: build ICMP echo request ---- */
    uint8_t*      buf    = send_buf;
    uint8_t*      p      = buf;
    uint32_t      icmp_data_len = 32;
    uint32_t      ip_data_len   = sizeof(icmp_header_t) + icmp_data_len;

    /* Ethernet header — send to dest_mac (which is gateway MAC for remote IPs) */
    p = build_ether_header(p, dest_mac, our_mac, ETHERTYPE_IP);

    /* IP header — destination IP is always the original target */
    p = build_ip_header(p, IP_PROTO_ICMP, OUR_IP, dest_ip, ip_data_len);

    /* ICMP header */
    icmp_header_t* icmp = (icmp_header_t*)p;
    icmp->type     = ICMP_ECHO_REQUEST;
    icmp->code     = 0;
    icmp->checksum = 0;
    icmp->id       = htons(0x1337);
    icmp->sequence = htons(icmp_seq++);
    p += sizeof(icmp_header_t);

    /* Payload: incrementing bytes (receiver can verify integrity) */
    for (uint32_t i = 0; i < icmp_data_len; i++)
        *p++ = (uint8_t)i;

    /* ICMP checksum covers header + data */
    icmp->checksum = checksum((uint16_t*)icmp, ip_data_len);

    net_send_ether(buf, sizeof(eth_header_t) + IP_HEADER_LEN + ip_data_len);
    pr_debug("sent echo request%s\n",
             is_local_ip(dest_ip) ? "" : " (via gateway)");
    return 0;
}

int net_get_ping_responses(void)
{
    return ping_responses;
}

/*
 * DNS query uses a transaction ID to match responses.  We keep a single
 * pending-query slot (serialised by the single-core design).
 */
static volatile int dns_pending   = 0;
static uint16_t     dns_xid       = 0;
static uint8_t      dns_reply[512];
static uint16_t     dns_reply_len = 0;

/* tcpdump-lite */

#define TCPDUMP_MAX 20
#define PACKET_MAX  2048

static volatile int tcpdump_active = 0;
static volatile int tcpdump_captured = 0;
static volatile int tcpdump_max = 5;
static volatile uint32_t tcpdump_cap_type = 0;
static uint32_t tcpdump_start_tick = 0;
static struct {
    uint32_t tick;
    uint16_t len;
    uint8_t  data[PACKET_MAX];
} tcpdump_buf[TCPDUMP_MAX];

int tcpdump_start(int count, int filter)
{
    if (count < 1 || count > TCPDUMP_MAX)
        return -1;
    tcpdump_captured = 0;
    tcpdump_max = count;
    tcpdump_cap_type = filter;
    tcpdump_start_tick = get_ticks();
    __asm__ volatile("" ::: "memory");
    tcpdump_active = 1;
    return 0;
}

int tcpdump_is_done(void)
{
    return !tcpdump_active;
}

int tcpdump_get_count(void)
{
    return tcpdump_captured;
}

static void tcpdump_capture(uint8_t* data, uint32_t len, uint16_t ethertype)
{
    if (!tcpdump_active)
        return;
    if (tcpdump_captured >= tcpdump_max)
        return;

    if (tcpdump_cap_type != 0) {
        int type_match = 0;
        if (tcpdump_cap_type == 1 && ethertype == ETHERTYPE_ARP) type_match = 1;
        if (tcpdump_cap_type == 2 && ethertype == ETHERTYPE_IP)  type_match = 1;
        if (!type_match) return;
    }

    int idx = tcpdump_captured;
    tcpdump_buf[idx].tick = get_ticks();
    if (len > PACKET_MAX)
        len = PACKET_MAX;
    tcpdump_buf[idx].len = (uint16_t)len;
    memcpy(tcpdump_buf[idx].data, data, len);
    __asm__ volatile("" ::: "memory");
    tcpdump_captured++;

    if (tcpdump_captured >= tcpdump_max)
        tcpdump_active = 0;
}

void tcpdump_print(void)
{
    char buf[16];
    terminal_writestring("=== tcpdump: ");
    int_to_string(tcpdump_captured, buf);
    terminal_writestring(buf);
    terminal_writestring(" packets captured ===\n");

    const char* hex = "0123456789ABCDEF";

    for (int i = 0; i < tcpdump_captured; i++) {
        uint32_t delta = tcpdump_buf[i].tick - tcpdump_start_tick;
        uint32_t secs = delta / 100;
        uint32_t msec = (delta % 100) * 10;
        uint16_t len = tcpdump_buf[i].len;
        uint8_t* d = tcpdump_buf[i].data;

        terminal_writestring("--- Packet ");
        int_to_string(i + 1, buf);
        terminal_writestring(buf);
        terminal_writestring(" (");
        int_to_string((int)len, buf);
        terminal_writestring(buf);
        terminal_writestring(" bytes) [");
        int_to_string((int)secs, buf);
        terminal_writestring(buf);
        terminal_putchar('.');
        if (msec < 100) terminal_putchar('0');
        if (msec < 10) terminal_putchar('0');
        int_to_string((int)msec, buf);
        terminal_writestring(buf);
        terminal_writestring("s] ---\n");

        for (uint32_t j = 0; j < len; j += 16) {
            format_hex((uint32_t)(uint32_t)&d[j], buf);
            terminal_writestring(buf);
            terminal_putchar(' ');

            for (uint32_t k = 0; k < 16; k++) {
                if (j + k < len) {
                    terminal_putchar(hex[(d[j + k] >> 4) & 0xF]);
                    terminal_putchar(hex[d[j + k] & 0xF]);
                } else {
                    terminal_writestring("  ");
                }
                terminal_putchar(' ');
                if (k == 7) terminal_putchar(' ');
            }
            terminal_putchar(' ');

            for (uint32_t k = 0; k < 16 && j + k < len; k++) {
                uint8_t c = d[j + k];
                terminal_putchar((c >= 32 && c < 127) ? (char)c : '.');
            }
            terminal_writestring("\n");
        }
    }

    tcpdump_captured = 0;
    tcpdump_active = 0;
}

/*
 * dns_handler - UDP callback for DNS responses.
 *
 * Called from handle_udp (IRQ context).  Validates the transaction ID,
 * then stashes the raw response and signals the waiter.
 *
 * DONE: Minimum-length check raised to 12 bytes (full DNS header).
 * DONE: XID read uses read_be16() to avoid strict-aliasing UB.
 */
static void dns_handler(uint32_t src_ip, uint16_t src_port,
                         const uint8_t* data, uint32_t len, void* userdata)
{
    (void)src_ip;
    (void)src_port;
    (void)userdata;

    pr_debug("handler called, len=%u\n", len);

    /* Need at least the 12-byte DNS header to validate */
    if (len < 12) {
        pr_debug("response too short\n");
        return;
    }

    uint16_t rx_xid = read_be16(data);
    if (rx_xid != dns_xid) {
        pr_debug("XID mismatch (rx=0x%x expected=0x%x)\n", rx_xid, dns_xid);
        return;
    }
    pr_debug("XID match, storing response\n");

    /* Stash the raw response and signal the waiter */
    if (len > sizeof(dns_reply))
        len = sizeof(dns_reply);
    memcpy(dns_reply, data, len);
    dns_reply_len = len;
    /* Barrier: dns_reply/dns_reply_len visible before dns_pending */
    __asm__ volatile("" ::: "memory");
    dns_pending = 1;
}

/*
 * dns_encode_name - Encode a hostname into DNS label format.
 *
 * "www.google.com" → "\x03www\x06google\x03com\x00"
 *
 * @param hostname  Null-terminated input hostname.
 * @param out       Output buffer for the encoded name.
 * @param out_len   Size of the output buffer.
 * @return          Number of bytes written, or -1 if buffer too small.
 */
static int dns_encode_name(const char* hostname, uint8_t* out, uint32_t out_len)
{
    uint32_t pos = 0;
    while (*hostname) {
        const char* dot = hostname;
        while (*dot && *dot != '.')
            dot++;
        uint32_t label_len = (uint32_t)(dot - hostname);
        if (label_len > 63)
            return -1;          /* Label too long for DNS */
        if (pos + label_len + 2 > out_len)
            return -1;          /* Buffer too small */
        out[pos++] = (uint8_t)label_len;
        for (uint32_t i = 0; i < label_len; i++)
            out[pos++] = (uint8_t)hostname[i];
        hostname = *dot ? dot + 1 : dot;
    }
    if (pos + 1 > out_len)
        return -1;
    out[pos++] = 0;             /* Root label */
    return (int)pos;
}

/*
 * dns_skip_name - Skip over a DNS name (handles compressed pointers).
 *
 * DNS names in responses may use pointer compression (0xC0 + offset).
 * This reads label bytes or pointer bytes and returns the offset just
 * past the name (or -1 on malformed data).
 */
static int dns_skip_name(const uint8_t* msg, uint32_t msg_len, uint32_t offset)
{
    while (offset < msg_len) {
        uint8_t label = msg[offset];
        if (label == 0)
            return (int)(offset + 1);           /* End of name */
        if ((label & 0xC0) == 0xC0)             /* Pointer (2 bytes) */
            return (int)(offset + 2);
        offset += 1 + label;                     /* Skip label + its bytes */
    }
    return -1;
}

/*
 * dns_parse_response - Extract the first A-record IP from a DNS response.
 *
 * Skips the header, question section, then walks answer records looking
 * for a type-A, class-IN record with a 4-byte RDATA.
 *
 * @param msg  Raw DNS response (UDP payload).
 * @param len  Length of the response.
 * @return     IP in network byte order, or 0 on failure.
 */
static uint32_t dns_parse_response(const uint8_t* msg, uint32_t len)
{
    /* Must at least fit the 12-byte header */
    if (len < 12)
        return 0;

    uint16_t flags         = read_be16(msg + 2);
    uint16_t qdcount       = read_be16(msg + 4);
    uint16_t ancount       = read_be16(msg + 6);

    /* Check for NXDOMAIN or other error (last 4 bits = rcode) */
    if ((flags & 0x000F) != 0)
        return 0;

    uint32_t offset = 12;

    /* Skip question section */
    for (uint16_t q = 0; q < qdcount; q++) {
        int ret = dns_skip_name(msg, len, offset);
        if (ret < 0) return 0;
        offset = (uint32_t)ret + 4;     /* Skip qtype + qclass */
        if (offset > len) return 0;
    }

    /* Walk answer records */
    for (uint16_t a = 0; a < ancount; a++) {
        int ret = dns_skip_name(msg, len, offset);
        if (ret < 0) return 0;
        offset = (uint32_t)ret;

        /* type(2) + class(2) + ttl(4) + rdlength(2) = 10 bytes fixed */
        if (offset + 10 > len) return 0;

        uint16_t type    = read_be16(msg + offset);
        uint16_t cls     = read_be16(msg + offset + 2);
        uint16_t rdlength = read_be16(msg + offset + 8);
        offset += 10;

        if (offset + rdlength > len) return 0;

        /* Found an A record: type=1, class=1, rdlength=4 */
        if (type == 1 && cls == 1 && rdlength == 4) {
            uint32_t ip;
            memcpy(&ip, msg + offset, 4);
            return ip;      /* Already network byte order */
        }

        offset += rdlength;
    }

    return 0;   /* No A record found */
}

/*
 * net_dns_resolve - Resolve a hostname to an IP address via DNS.
 *
 * Sends a standard DNS query (type A, class IN) to the configured
 * DNS server and busy-waits for a response.
 *
 * @param hostname  Null-terminated hostname.
 * @return          IP in network byte order, or 0 on failure.
 */
uint32_t net_dns_resolve(const char* hostname)
{
    /* ---- Build the DNS query ---- */
    union {
        uint8_t  u8[512];
        uint16_t u16[256];
    } query_u;
    uint8_t*  query  = query_u.u8;
    uint32_t  qlen   = 0;

    dns_xid++;
    query_u.u16[0] = htons(dns_xid);
    query_u.u16[1] = htons(0x0100);
    query_u.u16[2] = htons(1);
    query_u.u16[3] = 0;
    query_u.u16[4] = 0;
    query_u.u16[5] = 0;
    qlen = 12;

    int name_len = dns_encode_name(hostname, query + qlen, sizeof(query) - qlen - 4);
    if (name_len < 0)
        return 0;
    qlen += (uint32_t)name_len;

    uint16_t dns_qtype  = htons(1);
    uint16_t dns_qclass = htons(1);
    memcpy(query + qlen,     &dns_qtype,  2);
    memcpy(query + qlen + 2, &dns_qclass, 2);
    qlen += 4;

    if (verbose_net_flags & NETLOG_UDP)
        pr_debug("query len=%u\n", qlen);

    /*
     * Retry loop: try the full ARP + DNS sequence up to 3 times.
     * The first attempt may fail on slow QEMU slirp ARP resolution;
     * subsequent attempts reuse cached ARP and re-send the query.
     */
    uint32_t result = 0;

    for (int attempt = 0; attempt < 3 && result == 0; attempt++) {
        if (attempt > 0)
            pr_debug("retry %d\n", attempt + 1);

        dns_pending   = 0;
        dns_reply_len = 0;

        net_udp_register(DNS_CLIENT_PORT, dns_handler, NULL);
        int ret = net_send_udp(DNS_SERVER, DNS_PORT, DNS_CLIENT_PORT, query, qlen);

        if (ret < 0) {
            pr_debug("ARP not cached, sending request\n");
            uint8_t our_mac[6];
            rtl8139_get_mac(our_mac);
            uint8_t broadcast[ETH_ALEN] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
            uint8_t* buf = send_buf;
            uint8_t* p   = buf;

            p = build_ether_header(p, broadcast, our_mac, ETHERTYPE_ARP);

            arp_packet_t* ap = (arp_packet_t*)p;
            ap->htype = htons(ARP_HTYPE_ETHER);
            ap->ptype = htons(ARP_PROTO_IP);
            ap->hlen  = ETH_ALEN;
            ap->plen  = 4;
            ap->oper  = htons(ARP_OP_REQUEST);
            memcpy(ap->sha, our_mac, ETH_ALEN);
            ap->spa   = OUR_IP;
            memset(ap->tha, 0, ETH_ALEN);
            ap->tpa   = DNS_SERVER;

            pr_debug("sending ARP request for DNS server\n");
            net_send_ether(buf, sizeof(eth_header_t) + sizeof(arp_packet_t));

            /* Wait up to ~30 seconds for ARP reply, polling RX path actively */
            {
                int timeout = 3000;
                while (timeout-- > 0 && arp_find(DNS_SERVER) < 0) {
                    rtl8139_poll_rx();
                    sleep(10);
                }
            }

            if (arp_find(DNS_SERVER) < 0) {
                pr_debug("ARP resolution timed out after 30s\n");
                net_udp_unregister(DNS_CLIENT_PORT);
                continue;  /* Try next retry */
            }

            pr_debug("ARP resolved, sending query\n");

            dns_pending   = 0;
            dns_reply_len = 0;
            ret = net_send_udp(DNS_SERVER, DNS_PORT, DNS_CLIENT_PORT, query, qlen);
        }

        if (ret < 0) {
            pr_debug("UDP send failed even after ARP\n");
            net_udp_unregister(DNS_CLIENT_PORT);
            continue;
        }

        pr_debug("query sent, waiting for response\n");

        /* Wait up to ~20 seconds for the DNS response, polling RX */
        {
            int timeout = 2000;
            while (timeout-- > 0 && !dns_pending) {
                int npackets = rtl8139_poll_rx();
                if (npackets > 0)
                    pr_debug("polled %d packets during wait\n", npackets);
                sleep(10);
            }
        }

        if (dns_pending) {
            __asm__ volatile("" ::: "memory");
            pr_debug("response received\n");
            result = dns_parse_response((const uint8_t*)dns_reply, dns_reply_len);
            net_udp_unregister(DNS_CLIENT_PORT);
            break;  /* Success — exit retry loop */
        }

        pr_debug("response timed out\n");
        net_udp_unregister(DNS_CLIENT_PORT);
    }

    if (result == 0)
        pr_debug("all retries exhausted\n");

    return result;
}

void net_init(void)
{
    ping_responses = 0;
    ip_id   = 0;
    icmp_seq = 0;

    for (int i = 0; i < ARP_CACHE_SIZE; i++)
        arp_cache[i].valid = 0;

    for (int i = 0; i < UDP_MAX_HANDLERS; i++)
        udp_handlers[i].active = 0;

    dns_pending = 0;
    dns_xid = 0;

    pr_info("Stack initialized\n");
}
