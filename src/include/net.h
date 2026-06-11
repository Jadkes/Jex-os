/**
 * @file net.h
 * @brief Ethernet, ARP, IP, ICMP — L2/L3 networking for RTL8139.
 *
 * Protocol helpers (build_ether_header, build_ip_header) are public so UDP/TCP
 * can reuse them.  Two send buffers (send_buf for shell, reply_buf for ISR)
 * prevent re-entrancy between shell and interrupt reply paths.
 */
#ifndef NET_H
#define NET_H

#include <stdint.h>

/* Byte order — x86 is LE, network is BE */
#define htons(x) __builtin_bswap16(x)
#define ntohs(x) __builtin_bswap16(x)
#define htonl(x) __builtin_bswap32(x)
#define ntohl(x) __builtin_bswap32(x)

/* Ethernet */

#define ETH_ALEN      6
#define ETHERTYPE_ARP 0x0806
#define ETHERTYPE_IP  0x0800

typedef struct {
    uint8_t  dest[ETH_ALEN];
    uint8_t  src[ETH_ALEN];
    uint16_t type;
} __attribute__((packed)) eth_header_t;

/* ARP */

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
    uint8_t  sha[ETH_ALEN];     /* Sender hardware address          */
    uint32_t spa;               /* Sender protocol address          */
    uint8_t  tha[ETH_ALEN];     /* Target hardware address          */
    uint32_t tpa;               /* Target protocol address          */
} __attribute__((packed)) arp_packet_t;

/* IP */

#define IP_PROTO_ICMP 1
#define IP_PROTO_TCP  6
#define IP_PROTO_UDP  17

#define IP_HEADER_LEN 20

typedef struct {
    uint8_t  ver_ihl;           /* Version (4) + header length / 4 */
    uint8_t  dscp_ecn;          /* Differentiated Services + ECN   */
    uint16_t total_len;
    uint16_t id;
    uint16_t flags_frag;        /* Flags + fragment offset          */
    uint8_t  ttl;
    uint8_t  protocol;
    uint16_t checksum;
    uint32_t src_ip;
    uint32_t dest_ip;
} __attribute__((packed)) ip_header_t;

/* Shorthand: build IP address constant in network byte order */
#define IP4(a,b,c,d) htonl(((uint32_t)(a) << 24) | \
                           ((uint32_t)(b) << 16) | \
                           ((uint32_t)(c) << 8)  |  (d))

/* ICMP */

#define ICMP_ECHO_REPLY   0
#define ICMP_ECHO_REQUEST 8

typedef struct {
    uint8_t  type;
    uint8_t  code;
    uint16_t checksum;
    uint16_t id;
    uint16_t sequence;
} __attribute__((packed)) icmp_header_t;

/* UDP */

typedef struct {
    uint16_t src_port;
    uint16_t dest_port;
    uint16_t length;
    uint16_t checksum;
} __attribute__((packed)) udp_header_t;

/* UDP pseudo-header for checksum calculation (not on-wire) */
typedef struct {
    uint32_t src_ip;
    uint32_t dest_ip;
    uint8_t  zero;
    uint8_t  protocol;          /* IP_PROTO_UDP */
    uint16_t udp_len;
} __attribute__((packed)) udp_pseudo_t;

/* Buffer & Configuration */

#define PACKET_BUF_SIZE 2048

/* Our IP on the wire */
#define OUR_IP IP4(10, 0, 2, 15)

/* Default gateway for non-local traffic (QEMU slirp NAT) */
#define GATEWAY_IP IP4(10, 0, 2, 2)

/* DNS proxy (QEMU slirp) + ephemeral source port */
#define DNS_SERVER   IP4(10, 0, 2, 3)
#define DNS_PORT     53
#define DNS_CLIENT_PORT 54321

/* Protocol helpers */

uint16_t checksum(uint16_t* buf, uint32_t len);

/*
 * build_ether_header - Fill and advance past an Ethernet header.
 * @return pointer past the filled header (buf + sizeof(eth_header_t))
 */
uint8_t* build_ether_header(uint8_t* buf, const uint8_t* dest,
                            const uint8_t* src, uint16_t type);

/*
 * build_ip_header - Fill IP header (ver_ihl=0x45, TTL=64), compute checksum.
 * Sets total_len = IP_HEADER_LEN + data_len.  Caller fills payload after.
 * @return pointer past the filled header (buf + IP_HEADER_LEN)
 */
uint8_t* build_ip_header(uint8_t* buf, uint8_t protocol,
                         uint32_t src_ip, uint32_t dest_ip,
                         uint16_t data_len);

void net_send_ether(void* data, uint32_t len);

/* ARP cache */
int arp_lookup(uint32_t ip);
void arp_dump(void);

/* Routing */
int  is_local_ip(uint32_t ip);
int  resolve_mac(uint32_t ip, uint8_t* mac_out);
void net_arp_resolve(uint32_t ip);
void route_print(void);

/* TCP integration — called from handle_ip() when IP_PROTO_TCP */
void handle_tcp(uint8_t* data, uint32_t len, uint32_t ip_hdr,
                const uint8_t* src_mac);

/* Public API */

#define NETLOG_OFF  0
#define NETLOG_ARP  (1 << 0)
#define NETLOG_IP   (1 << 1)
#define NETLOG_ICMP (1 << 2)
#define NETLOG_UDP  (1 << 3)
#define NETLOG_ALL  (NETLOG_ARP | NETLOG_IP | NETLOG_ICMP | NETLOG_UDP)

void netlog_set_flags(int flags);
void net_init(void);
void net_process_packet(uint8_t* data, uint32_t len);
int  net_ping(uint32_t dest_ip);
int  net_get_ping_responses(void);

/** tcpdump-lite: start capture, poll is_done, print hex dump */
int  tcpdump_start(int count, int filter);
int  tcpdump_is_done(void);
int  tcpdump_get_count(void);
void tcpdump_print(void);

#define TCPDUMP_ALL  0
#define TCPDUMP_ARP  1
#define TCPDUMP_IP   2

/* DNS — sends A-record query via UDP, busy-waits for response */
uint32_t net_dns_resolve(const char* hostname);

/* UDP */

typedef void (*udp_callback_t)(uint32_t src_ip, uint16_t src_port,
                                const uint8_t* data, uint32_t len,
                                void* userdata);

int  net_udp_register(uint16_t port, udp_callback_t handler, void* userdata);
void net_udp_unregister(uint16_t port);

/*
 * net_send_udp - Send a UDP datagram.
 * Resolves destination via ARP cache; returns -1 if ARP not resolved.
 * Computes UDP checksum with pseudo-header (RFC 768).
 */
int net_send_udp(uint32_t dest_ip, uint16_t dest_port, uint16_t src_port,
                 const uint8_t* data, uint32_t len);

#endif /* NET_H */
