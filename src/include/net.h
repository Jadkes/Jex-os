/**
 * @file net.h
 * @brief Network Stack — Ethernet, ARP, IP, ICMP for JexOS.
 *
 * Purpose: Provides L2/L3 networking on top of the RTL8139 NIC driver.
 *          Demuxes Ethernet frames, maintains an ARP cache, verifies and
 *          routes IP packets, and handles ICMP echo (ping).
 *
 * Design: Protocol-construction helpers (build_ether_header, build_ip_header)
 *         are exposed so upper-layer protocols (UDP, TCP) reuse them without
 *         duplicating header assembly. A common checksum() serves IP, ICMP,
 *         and eventually UDP (with pseudo-header).
 *
 *         Two independent send buffers prevent re-entrancy between the
 *         shell context path and the interrupt reply path.
 *
 * Thread-safety: Called from both shell context and RTL8139 IRQ handler.
 *                send_buf is for shell-initiated transmits.
 *                reply_buf is for interrupt-driven replies (ARP, ICMP).
 *                They MUST NOT be used from the wrong context.
 */

#ifndef NET_H
#define NET_H

#include <stdint.h>

/* ----------------------------------------------------------------- */
/*  Byte Order                                                       */
/* ----------------------------------------------------------------- */

/* x86 is little-endian; network is big-endian. */
#define htons(x) __builtin_bswap16(x)
#define ntohs(x) __builtin_bswap16(x)
#define htonl(x) __builtin_bswap32(x)
#define ntohl(x) __builtin_bswap32(x)

/* ----------------------------------------------------------------- */
/*  Ethernet                                                         */
/* ----------------------------------------------------------------- */

#define ETH_ALEN      6           /* Octets in a MAC address         */
#define ETHERTYPE_ARP 0x0806     /* Payload = ARP packet            */
#define ETHERTYPE_IP  0x0800     /* Payload = IP datagram           */

typedef struct {
    uint8_t  dest[ETH_ALEN];
    uint8_t  src[ETH_ALEN];
    uint16_t type;
} __attribute__((packed)) eth_header_t;

/* ----------------------------------------------------------------- */
/*  ARP                                                              */
/* ----------------------------------------------------------------- */

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

/* ----------------------------------------------------------------- */
/*  IP                                                               */
/* ----------------------------------------------------------------- */

#define IP_PROTO_ICMP 1
#define IP_PROTO_TCP  6
#define IP_PROTO_UDP  17

/* Minimum IP header length (no options) */
#define IP_HEADER_LEN 20

typedef struct {
    uint8_t  ver_ihl;           /* Version (4) + header length / 4 */
    uint8_t  dscp_ecn;          /* Differentiated Services + ECN   */
    uint16_t total_len;         /* Entire datagram length           */
    uint16_t id;                /* Fragment identification          */
    uint16_t flags_frag;        /* Flags + fragment offset          */
    uint8_t  ttl;               /* Time to live                     */
    uint8_t  protocol;          /* Upper-layer protocol             */
    uint16_t checksum;          /* Header checksum                  */
    uint32_t src_ip;            /* Source IP (network order)        */
    uint32_t dest_ip;           /* Destination IP (network order)   */
} __attribute__((packed)) ip_header_t;

/* Shorthand: build an IP address constant in network byte order. */
#define IP4(a,b,c,d) htonl(((uint32_t)(a) << 24) | \
                           ((uint32_t)(b) << 16) | \
                           ((uint32_t)(c) << 8)  |  (d))

/* ----------------------------------------------------------------- */
/*  ICMP                                                             */
/* ----------------------------------------------------------------- */

#define ICMP_ECHO_REPLY   0
#define ICMP_ECHO_REQUEST 8

typedef struct {
    uint8_t  type;
    uint8_t  code;
    uint16_t checksum;
    uint16_t id;
    uint16_t sequence;
} __attribute__((packed)) icmp_header_t;

/* ----------------------------------------------------------------- */
/*  UDP                                                              */
/* ----------------------------------------------------------------- */

typedef struct {
    uint16_t src_port;
    uint16_t dest_port;
    uint16_t length;            /* Header + data length */
    uint16_t checksum;          /* Pseudo-header + data (0 = no checksum) */
} __attribute__((packed)) udp_header_t;

/*
 * UDP pseudo-header prepended to UDP datagrams for checksum calculation.
 * Not a real on-wire structure — exists only for checksum() input.
 */
typedef struct {
    uint32_t src_ip;
    uint32_t dest_ip;
    uint8_t  zero;
    uint8_t  protocol;          /* IP_PROTO_UDP */
    uint16_t udp_len;
} __attribute__((packed)) udp_pseudo_t;

/* ----------------------------------------------------------------- */
/*  Buffer & Configuration                                           */
/* ----------------------------------------------------------------- */

/*
 * Maximum size for a single packet we construct.
 * Must be large enough to hold Ethernet + IP + any payload.
 */
#define PACKET_BUF_SIZE 2048

/* Our IP address on the wire (network byte order). */
#define OUR_IP IP4(10, 0, 2, 15)

/* Default DNS server (QEMU user-mode gateway provides a DNS proxy). */
#define DNS_SERVER   IP4(10, 0, 2, 3)
#define DNS_PORT     53

/* Ephemeral source port for DNS queries (responses arrive here). */
#define DNS_CLIENT_PORT 54321

/* ----------------------------------------------------------------- */
/*  Protocol Helpers — shared by net.c and future protocols          */
/* ----------------------------------------------------------------- */

/*
 * checksum - 16-bit one's complement checksum over len bytes.
 *
 * WHY: IP, ICMP, and UDP all need the same algorithm.
 *      Callers are responsible for zeroing the checksum field before
 *      calling if they want to compute it over a partial header.
 *
 * @param buf  Pointer to the data to checksum (treated as 16-bit words).
 * @param len  Length in bytes (may be odd; the trailing byte is folded in).
 * @return     The 16-bit one's complement checksum.
 */
uint16_t checksum(uint16_t* buf, uint32_t len);

/*
 * build_ether_header - Fill an Ethernet header at buf and advance past it.
 *
 * @param buf      Destination buffer.
 * @param dest     6-byte destination MAC.
 * @param src      6-byte source MAC.
 * @param type     EtherType (ETHERTYPE_IP, ETHERTYPE_ARP, ...).
 * @return         Pointer past the filled header (buf + sizeof(eth_header_t)).
 */
uint8_t* build_ether_header(uint8_t* buf, const uint8_t* dest,
                            const uint8_t* src, uint16_t type);

/*
 * build_ip_header - Fill an IP header at buf, compute checksum, advance.
 *
 * Fills ver_ihl=0x45, TTL=64, increments ip_id internally.
 * The caller must fill total_len and payload after calling.
 *
 * @param buf       Destination buffer.
 * @param protocol  Upper-layer protocol (IP_PROTO_ICMP, IP_PROTO_UDP, ...).
 * @param src_ip    Source IP (network order).
 * @param dest_ip   Destination IP (network order).
 * @param data_len  Length of payload after the IP header.
 * @return          Pointer past the filled header (buf + IP_HEADER_LEN).
 */
uint8_t* build_ip_header(uint8_t* buf, uint8_t protocol,
                         uint32_t src_ip, uint32_t dest_ip,
                         uint16_t data_len);

/*
 * net_send_ether - Send a raw Ethernet frame via the NIC.
 *
 * Thin wrapper around the NIC driver's send so upper layers don't need
 * to depend on rtl8139.h directly.
 */
void net_send_ether(void* data, uint32_t len);

/*
 * arp_lookup - Find cached MAC for an IP, or -1 if not cached.
 *
 * Used by upper layers (UDP, TCP) to resolve destinations before sending.
 */
int arp_lookup(uint32_t ip);

/*
 * arp_dump - Print the contents of the ARP cache.
 */
void arp_dump(void);

/* ----------------------------------------------------------------- */
/*  Public API                                                       */
/* ----------------------------------------------------------------- */

void net_init(void);
void net_process_packet(uint8_t* data, uint32_t len);
int  net_ping(uint32_t dest_ip);
int  net_get_ping_responses(void);

/* ----------------------------------------------------------------- */
/*  DNS API                                                          */
/* ----------------------------------------------------------------- */

/*
 * net_dns_resolve - Resolve a hostname to an IP address via DNS.
 *
 * Sends a DNS A-record query to the configured DNS server (UDP port 53),
 * busy-waits for a response, and returns the first A record found.
 *
 * The hostname must be a null-terminated string (e.g. "google.com").
 * Returns 0 on failure (timeout, NXDOMAIN, malformed response).
 *
 * @param hostname  Null-terminated hostname to resolve.
 * @return          IP address in network byte order, or 0 on failure.
 */
uint32_t net_dns_resolve(const char* hostname);

/* ----------------------------------------------------------------- */
/*  UDP API                                                          */
/* ----------------------------------------------------------------- */

/*
 * Callback invoked when a UDP datagram arrives for a registered port.
 *
 * @param src_ip   Source IP (network byte order).
 * @param src_port Source port (network byte order).
 * @param data     Pointer to the UDP payload (after UDP header).
 * @param len      Length of the payload in bytes.
 * @param userdata Opaque pointer supplied at registration time.
 */
typedef void (*udp_callback_t)(uint32_t src_ip, uint16_t src_port,
                                const uint8_t* data, uint32_t len,
                                void* userdata);

/*
 * net_udp_register - Register a handler for an incoming UDP port.
 *
 * Multiple registrations to the same port overwrite the previous one.
 *
 * @param port     Port in host byte order.
 * @param handler  Callback to invoke on datagram arrival (IRQ context!).
 * @param userdata Opaque pointer passed back to the callback.
 * @return         0 on success, -1 if the handler table is full.
 */
int net_udp_register(uint16_t port, udp_callback_t handler, void* userdata);

/*
 * net_udp_unregister - Remove a UDP handler for a port.
 *
 * @param port  Port in host byte order.
 */
void net_udp_unregister(uint16_t port);

/*
 * net_send_udp - Send a UDP datagram.
 *
 * Resolves the destination via ARP cache.  If the IP is not cached,
 * returns -1 (caller should retry after ARP resolution).
 *
 * Computes the UDP checksum with the pseudo-header (RFC 768).
 *
 * @param dest_ip  Destination IP (network byte order).
 * @param dest_port Destination port (host byte order).
 * @param src_port  Source port (host byte order).
 * @param data     Payload data.
 * @param len      Payload length in bytes.
 * @return         0 on success, -1 if ARP not resolved.
 */
int net_send_udp(uint32_t dest_ip, uint16_t dest_port, uint16_t src_port,
                 const uint8_t* data, uint32_t len);

#endif /* NET_H */
