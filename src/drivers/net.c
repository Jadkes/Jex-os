/**
 * @file net.c
 * @brief Network Stack Implementation.
 *
 * Sits on top of the RTL8139 NIC driver. Provides Ethernet demux,
 * ARP cache/reply, IP receive, and ICMP echo (ping) handling.
 *
 * Uses separate send buffers for shell-initiated transmits vs
 * interrupt-handler replies to avoid re-entrancy issues.
 */

#include "net.h"
#include "rtl8139.h"
#include "terminal.h"
#include "serial.h"
#include "kheap.h"
#include <string.h>

/* ---- TX Buffers ---- */
/* Separate buffers: shell path vs IRQ reply path */
static uint8_t send_buf[2048];
static uint8_t reply_buf[2048];

/* ---- ARP Cache (4 entries) ---- */
#define ARP_CACHE_SIZE 4

typedef struct {
    uint32_t ip;
    uint8_t  mac[6];
    int      valid;
} arp_entry_t;

static arp_entry_t arp_cache[ARP_CACHE_SIZE];

/* ---- State ---- */
static int      ping_responses = 0;
static uint16_t ip_id          = 0;
static uint16_t icmp_seq       = 0;

/*
 * 16-bit one's complement checksum over len bytes.
 * Used for both IP headers and ICMP packets.
 */
static uint16_t checksum(uint16_t* buf, uint32_t len)
{
    uint32_t sum = 0;
    for (uint32_t i = 0; i < len / 2; i++)
        sum += buf[i];
    if (len & 1)
        sum += ((uint8_t*)buf)[len - 1];
    while (sum >> 16)
        sum = (sum & 0xFFFF) + (sum >> 16);
    return ~(uint16_t)sum & 0xFFFF;
}

/* ---- ARP Cache Helpers ---- */

static int arp_find(uint32_t ip)
{
    for (int i = 0; i < ARP_CACHE_SIZE; i++)
        if (arp_cache[i].valid && arp_cache[i].ip == ip)
            return i;
    return -1;
}

static void arp_add(uint32_t ip, uint8_t* mac)
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
    /* Evict oldest */
    for (int i = 1; i < ARP_CACHE_SIZE; i++)
        arp_cache[i - 1] = arp_cache[i];
    arp_cache[ARP_CACHE_SIZE - 1].ip = ip;
    memcpy(arp_cache[ARP_CACHE_SIZE - 1].mac, mac, 6);
    arp_cache[ARP_CACHE_SIZE - 1].valid = 1;
}

/* ---- ARP Packet Handler ---- */

static void handle_arp(uint8_t* data, uint32_t len)
{
    if (len < sizeof(arp_packet_t))
        return;
    arp_packet_t* ap = (arp_packet_t*)data;

    uint16_t oper = ntohs(ap->oper);
    if (ntohs(ap->htype) != ARP_HTYPE_ETHER ||
        ntohs(ap->ptype) != ARP_PROTO_IP)
        return;
    if (ap->hlen != 6 || ap->plen != 4)
        return;

    arp_add(ap->spa, ap->sha);

    if (oper == ARP_OP_REQUEST && ap->tpa == OUR_IP) {
        uint8_t our_mac[6];
        rtl8139_get_mac(our_mac);

        uint8_t* buf = reply_buf;

        eth_header_t* eth = (eth_header_t*)buf;
        memcpy(eth->dest, ap->sha, 6);
        memcpy(eth->src,  our_mac, 6);
        eth->type = htons(ETHERTYPE_ARP);

        arp_packet_t* reply = (arp_packet_t*)(buf + sizeof(eth_header_t));
        reply->htype = htons(ARP_HTYPE_ETHER);
        reply->ptype = htons(ARP_PROTO_IP);
        reply->hlen  = 6;
        reply->plen  = 4;
        reply->oper  = htons(ARP_OP_REPLY);
        memcpy(reply->sha, our_mac, 6);
        reply->spa = OUR_IP;
        memcpy(reply->tha, ap->sha, 6);
        reply->tpa = ap->spa;

        rtl8139_send_packet(buf, sizeof(eth_header_t) + sizeof(arp_packet_t));

        log_serial("ARP: replied\n");
    }
}

/* ---- ICMP Packet Handler ---- */

static void handle_icmp(uint8_t* data, uint32_t len, uint8_t* src_mac)
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

    if (icmp->type == ICMP_ECHO_REQUEST && ip->dest_ip == OUR_IP) {
        uint8_t our_mac[6];
        rtl8139_get_mac(our_mac);

        uint8_t* buf    = reply_buf;
        uint32_t total  = sizeof(eth_header_t) + ip_tot;
        if (total > sizeof(reply_buf))
            return;

        eth_header_t* eth = (eth_header_t*)buf;
        memcpy(eth->dest, src_mac, 6);
        memcpy(eth->src,  our_mac, 6);
        eth->type = htons(ETHERTYPE_IP);

        /* Copy the IP datagram */
        memcpy(buf + sizeof(eth_header_t), data, ip_tot);

        /* Fix IP header: swap src/dest, recalculate checksum */
        ip_header_t* rip = (ip_header_t*)(buf + sizeof(eth_header_t));
        rip->dest_ip  = ip->src_ip;
        rip->src_ip   = OUR_IP;
        rip->checksum = 0;
        rip->checksum = checksum((uint16_t*)rip, ip_hdr);

        /* Fix ICMP: Echo Request → Echo Reply, recalc checksum */
        icmp_header_t* ricmp = (icmp_header_t*)(buf + sizeof(eth_header_t) + ip_hdr);
        ricmp->type    = ICMP_ECHO_REPLY;
        ricmp->code    = 0;
        ricmp->checksum = 0;
        ricmp->checksum = checksum((uint16_t*)ricmp, icmp_len);

        rtl8139_send_packet(buf, total);

        log_serial("ICMP: echo reply sent\n");
    }

    if (icmp->type == ICMP_ECHO_REPLY && ip->dest_ip == OUR_IP) {
        ping_responses++;
        log_serial("ICMP: echo reply received\n");
    }
}

/* ---- IP Packet Handler ---- */

static void handle_ip(uint8_t* data, uint32_t len, uint8_t* src_mac)
{
    if (len < sizeof(ip_header_t)) {
        log_serial("IP: too short\n");
        return;
    }
    ip_header_t* ip = (ip_header_t*)data;
    uint32_t ip_hdr = (ip->ver_ihl & 0x0F) * 4;
    if (len < ip_hdr) {
        log_serial("IP: hdr longer than data\n");
        return;
    }

    /* Verify IP checksum */
    uint16_t saved = ip->checksum;
    ip->checksum = 0;
    if (checksum((uint16_t*)ip, ip_hdr) != saved) {
        log_serial("IP: bad checksum\n");
        ip->checksum = saved;  /* restore before returning */
        return;
    }
    ip->checksum = saved;

    /* Learn sender MAC (needed for replies) */
    arp_add(ip->src_ip, src_mac);

    /* Only handle packets addressed to us */
    if (ip->dest_ip != OUR_IP)
        return;

    if (ip->protocol == IP_PROTO_ICMP)
        handle_icmp(data, len, src_mac);
}

/* ---- Public: Called from NIC IRQ handler ---- */

void net_process_packet(uint8_t* data, uint32_t len)
{
    if (len < sizeof(eth_header_t))
        return;

    eth_header_t* eth  = (eth_header_t*)data;
    uint16_t type      = ntohs(eth->type);
    uint8_t* payload   = data + sizeof(eth_header_t);
    uint32_t plen      = len - sizeof(eth_header_t);

    switch (type) {
    case ETHERTYPE_ARP:
        handle_arp(payload, plen);
        break;
    case ETHERTYPE_IP:
        handle_ip(payload, plen, eth->src);
        break;
    }
}

/* ---- Public: Send an ICMP Echo Request (ping) ---- */

int net_ping(uint32_t dest_ip)
{
    uint8_t our_mac[6];
    rtl8139_get_mac(our_mac);

    int arp_idx = arp_find(dest_ip);
    if (arp_idx < 0) {
        /* ARP not cached – send a request first */
        uint8_t* buf = send_buf;

        eth_header_t* eth = (eth_header_t*)buf;
        memset(eth->dest, 0xFF, 6);
        memcpy(eth->src, our_mac, 6);
        eth->type = htons(ETHERTYPE_ARP);

        arp_packet_t* ap = (arp_packet_t*)(buf + sizeof(eth_header_t));
        ap->htype = htons(ARP_HTYPE_ETHER);
        ap->ptype = htons(ARP_PROTO_IP);
        ap->hlen  = 6;
        ap->plen  = 4;
        ap->oper  = htons(ARP_OP_REQUEST);
        memcpy(ap->sha, our_mac, 6);
        ap->spa = OUR_IP;
        memset(ap->tha, 0, 6);
        ap->tpa = dest_ip;

        rtl8139_send_packet(buf, sizeof(eth_header_t) + sizeof(arp_packet_t));
        log_serial("PING: ARP request sent\n");
        return -1;
    }

    /* Build ICMP echo request */
    uint8_t* buf    = send_buf;
    uint32_t total  = 0;
    uint32_t icmp_data_len = 32;

    /* Ethernet header */
    eth_header_t* eth = (eth_header_t*)buf;
    memcpy(eth->dest, arp_cache[arp_idx].mac, 6);
    memcpy(eth->src,  our_mac, 6);
    eth->type = htons(ETHERTYPE_IP);
    total += sizeof(eth_header_t);

    /* IP header */
    ip_header_t* ip = (ip_header_t*)(buf + total);
    ip->ver_ihl   = 0x45;
    ip->dscp_ecn  = 0;
    ip->total_len = htons(20 + 8 + icmp_data_len);
    ip->id        = htons(ip_id++);
    ip->flags_frag = 0;
    ip->ttl       = 64;
    ip->protocol  = IP_PROTO_ICMP;
    ip->checksum  = 0;
    ip->src_ip    = OUR_IP;
    ip->dest_ip   = dest_ip;
    ip->checksum  = checksum((uint16_t*)ip, 20);
    total += 20;

    /* ICMP header */
    icmp_header_t* icmp = (icmp_header_t*)(buf + total);
    icmp->type     = ICMP_ECHO_REQUEST;
    icmp->code     = 0;
    icmp->checksum = 0;
    icmp->id       = htons(0x1337);
    icmp->sequence = htons(icmp_seq++);
    total += 8;

    /* Payload data (checked by the receiver for corruption) */
    for (uint32_t i = 0; i < icmp_data_len; i++)
        buf[total++] = i;

    /* ICMP checksum covers header + data */
    icmp->checksum = checksum((uint16_t*)icmp, 8 + icmp_data_len);

    rtl8139_send_packet(buf, total);
    log_serial("PING: sent echo request\n");
    return 0;
}

int net_get_ping_responses(void)
{
    return ping_responses;
}

void net_init(void)
{
    ping_responses = 0;
    ip_id  = 0;
    icmp_seq = 0;
    for (int i = 0; i < ARP_CACHE_SIZE; i++)
        arp_cache[i].valid = 0;
    log_serial("NET: Stack initialized\n");
}
