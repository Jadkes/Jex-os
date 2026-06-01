/**
 * @file net.h
 * @brief Network Stack - ARP, IP, ICMP for JexOS.
 *
 * Sits on top of the RTL8139 NIC driver. Handles Ethernet demux,
 * ARP cache/reply, and ICMP echo (ping).
 */

#ifndef NET_H
#define NET_H

#include <stdint.h>

/* Byte order (x86 is LE, network is BE) */
#define htons(x) __builtin_bswap16(x)
#define ntohs(x) __builtin_bswap16(x)
#define htonl(x) __builtin_bswap32(x)
#define ntohl(x) __builtin_bswap32(x)

/* ---- Ethernet ---- */
#define ETH_ALEN      6
#define ETHERTYPE_ARP 0x0806
#define ETHERTYPE_IP  0x0800

typedef struct {
    uint8_t  dest[ETH_ALEN];
    uint8_t  src[ETH_ALEN];
    uint16_t type;
} __attribute__((packed)) eth_header_t;

/* ---- ARP ---- */
#define ARP_HTYPE_ETHER 1
#define ARP_PROTO_IP    0x0800
#define ARP_OP_REQUEST  1
#define ARP_OP_REPLY    2

typedef struct {
    uint16_t htype;
    uint16_t ptype;
    uint8_t  hlen;
    uint8_t  plen;
    uint16_t oper;
    uint8_t  sha[ETH_ALEN];
    uint32_t spa;
    uint8_t  tha[ETH_ALEN];
    uint32_t tpa;
} __attribute__((packed)) arp_packet_t;

/* ---- IP ---- */
#define IP_PROTO_ICMP 1

typedef struct {
    uint8_t  ver_ihl;
    uint8_t  dscp_ecn;
    uint16_t total_len;
    uint16_t id;
    uint16_t flags_frag;
    uint8_t  ttl;
    uint8_t  protocol;
    uint16_t checksum;
    uint32_t src_ip;
    uint32_t dest_ip;
} __attribute__((packed)) ip_header_t;

/* Shorthand: IP in network byte order */
#define IP4(a,b,c,d) htonl(((uint32_t)(a) << 24) | \
                           ((uint32_t)(b) << 16) | \
                           ((uint32_t)(c) << 8)  |  (d))

/* ---- ICMP ---- */
#define ICMP_ECHO_REPLY   0
#define ICMP_ECHO_REQUEST 8

typedef struct {
    uint8_t  type;
    uint8_t  code;
    uint16_t checksum;
    uint16_t id;
    uint16_t sequence;
} __attribute__((packed)) icmp_header_t;

/* ---- Configuration ---- */
#define OUR_IP IP4(10, 0, 2, 15)

/* ---- API ---- */
void net_init(void);
void net_process_packet(uint8_t* data, uint32_t len);
int  net_ping(uint32_t dest_ip);
int  net_get_ping_responses(void);

#endif
