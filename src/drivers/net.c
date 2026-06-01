/**
 * @file net.c
 * @brief Network Stack Implementation — ARP, IP, ICMP.
 *
 * Purpose: Provides L2/L3 networking on top of the RTL8139 NIC driver.
 *          Sits between the NIC's interrupt handler and higher protocols.
 *
 * Design: Protocol helpers (build_ether_header, build_ip_header) are
 *         factored out so that UDP/TCP can be added without duplicating
 *         header construction. The IP demux dispatches by protocol field
 *         — adding a new L4 protocol means adding a handler call.
 *
 *         Two independent send buffers prevent re-entrancy issues:
 *           send_buf  — shell/user context  (net_ping, future UDP send)
 *           reply_buf — IRQ context         (ARP reply, ICMP echo reply)
 *
 * Thread-safety: The interrupt handler (rtl8139_handler) calls
 *                net_process_packet() which may trigger reply_buf usage.
 *                Shell code uses send_buf. Never cross the streams.
 */

#include "net.h"
#include "rtl8139.h"
#include "terminal.h"
#include "serial.h"
#include "kheap.h"
#include <string.h>

/* ----------------------------------------------------------------- */
/*  TX Buffers                                                       */
/* ----------------------------------------------------------------- */

/* shell / user-initiated transmits */
static uint8_t send_buf[PACKET_BUF_SIZE];

/* interrupt-reply-path transmits (ARP reply, ICMP echo reply) */
static uint8_t reply_buf[PACKET_BUF_SIZE];

/* ----------------------------------------------------------------- */
/*  ARP Cache (4 entries, FIFO eviction)                             */
/* ----------------------------------------------------------------- */

#define ARP_CACHE_SIZE 4

typedef struct {
    uint32_t ip;
    uint8_t  mac[6];
    int      valid;
} arp_entry_t;

static arp_entry_t arp_cache[ARP_CACHE_SIZE];

/* ----------------------------------------------------------------- */
/*  State                                                            */
/* ----------------------------------------------------------------- */

static int      ping_responses = 0;
static uint16_t ip_id          = 0;    /* Monotonic IP identification */
static uint16_t icmp_seq       = 0;

/* ----------------------------------------------------------------- */
/*  Checksum                                                         */
/* ----------------------------------------------------------------- */

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

/* ----------------------------------------------------------------- */
/*  ARP Cache Helpers                                                */
/* ----------------------------------------------------------------- */

/*
 * arp_find - Search the cache for a given IP.
 * @param ip  Target IP (network byte order).
 * @return    Index into arp_cache[], or -1 if not found.
 */
static int arp_find(uint32_t ip)
{
    for (int i = 0; i < ARP_CACHE_SIZE; i++)
        if (arp_cache[i].valid && arp_cache[i].ip == ip)
            return i;
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
            arp_cache[i].valid = 1;
            return;
        }
    }
    /* Evict oldest (shift left, insert at end) */
    for (int i = 1; i < ARP_CACHE_SIZE; i++)
        arp_cache[i - 1] = arp_cache[i];
    arp_cache[ARP_CACHE_SIZE - 1].ip = ip;
    memcpy(arp_cache[ARP_CACHE_SIZE - 1].mac, mac, 6);
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

/* ----------------------------------------------------------------- */
/*  Protocol Helpers                                                 */
/* ----------------------------------------------------------------- */

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
    rtl8139_send_packet(data, len);
}

/* ----------------------------------------------------------------- */
/*  ARP Packet Handler                                               */
/* ----------------------------------------------------------------- */

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

    arp_packet_t* ap = (arp_packet_t*)data;
    uint16_t oper = ntohs(ap->oper);

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
    log_serial("ARP: replied\n");
}

/* ----------------------------------------------------------------- */
/*  UDP Handler (stub — filled when UDP is implemented)              */
/* ----------------------------------------------------------------- */

/*
 * handle_udp - Process an incoming UDP datagram.
 *
 * TODO: Parse the UDP header, validate length/checksum, look up the
 *       destination port in a registered-handler table, and dispatch.
 *
 * @param data    Pointer past Ethernet header (IP header starts here).
 * @param len     Remaining packet length from IP header onward.
 * @param ip_hdr  Length of the IP header (for locating UDP header).
 */
static void handle_udp(uint8_t* data, uint32_t len, uint32_t ip_hdr)
{
    (void)data;
    (void)len;
    (void)ip_hdr;
    /* No-op until UDP handlers are registered */
}

/* ----------------------------------------------------------------- */
/*  ICMP Packet Handler                                              */
/* ----------------------------------------------------------------- */

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
        log_serial("ICMP: echo reply sent\n");
    }

    /* ---- Echo Reply: count it ---- */
    if (icmp->type == ICMP_ECHO_REPLY && ip->dest_ip == OUR_IP) {
        ping_responses++;
        log_serial("ICMP: echo reply received\n");
    }
}

/* ----------------------------------------------------------------- */
/*  IP Packet Handler                                                */
/* ----------------------------------------------------------------- */

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
        log_serial("IP: too short\n");
        return;
    }

    ip_header_t* ip   = (ip_header_t*)data;
    uint32_t     ip_hdr = (ip->ver_ihl & 0x0F) * 4;

    if (len < ip_hdr) {
        log_serial("IP: hdr longer than data\n");
        return;
    }

    /* Verify header checksum */
    uint16_t saved = ip->checksum;
    ip->checksum = 0;
    if (checksum((uint16_t*)ip, ip_hdr) != saved) {
        log_serial("IP: bad checksum\n");
        ip->checksum = saved;       /* restore before discard */
        return;
    }
    ip->checksum = saved;

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
    /* TODO: case IP_PROTO_TCP: handle_tcp(data, len, src_mac); */
    }
}

/* ----------------------------------------------------------------- */
/*  Public: Called from NIC IRQ handler                              */
/* ----------------------------------------------------------------- */

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

    switch (type) {
    case ETHERTYPE_ARP:
        handle_arp(payload, plen);
        break;
    case ETHERTYPE_IP:
        handle_ip(payload, plen, eth->src);
        break;
    }
}

/* ----------------------------------------------------------------- */
/*  Public: Send an ICMP Echo Request (ping)                         */
/* ----------------------------------------------------------------- */

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

    /* ---- ARP not cached: broadcast an ARP request ---- */
    int arp_idx = arp_find(dest_ip);
    if (arp_idx < 0) {
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
        ap->tpa   = dest_ip;

        net_send_ether(buf, sizeof(eth_header_t) + sizeof(arp_packet_t));
        log_serial("PING: ARP request sent\n");
        return -1;
    }

    /* ---- ARP cached: build ICMP echo request ---- */
    uint8_t*      buf    = send_buf;
    uint8_t*      p      = buf;
    uint32_t      icmp_data_len = 32;
    uint32_t      ip_data_len   = sizeof(icmp_header_t) + icmp_data_len;

    /* Ethernet header */
    p = build_ether_header(p, arp_cache[arp_idx].mac, our_mac, ETHERTYPE_IP);

    /* IP header pointing at the ICMP payload */
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
    log_serial("PING: sent echo request\n");
    return 0;
}

/* ----------------------------------------------------------------- */
/*  Public: Ping response counter                                    */
/* ----------------------------------------------------------------- */

int net_get_ping_responses(void)
{
    return ping_responses;
}

/* ----------------------------------------------------------------- */
/*  Initialization                                                   */
/* ----------------------------------------------------------------- */

void net_init(void)
{
    ping_responses = 0;
    ip_id   = 0;
    icmp_seq = 0;

    for (int i = 0; i < ARP_CACHE_SIZE; i++)
        arp_cache[i].valid = 0;

    log_serial("NET: Stack initialized\n");
}
