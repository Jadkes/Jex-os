/**
 * @file dhcp.h
 * @brief DHCP client protocol — DORA over UDP.
 *
 * Broadcasts on UDP port 68, listens for port 67 replies via the
 * existing UDP handler infrastructure.  Updates our_ip, gateway_ip,
 * and dns_server on success.
 */
#ifndef DHCP_H
#define DHCP_H

#include <stdint.h>

/* DHCP standard ports */
#define DHCP_SERVER_PORT  67
#define DHCP_CLIENT_PORT  68

/* DHCP message types (option 53) */
#define DHCP_DISCOVER  1
#define DHCP_OFFER     2
#define DHCP_REQUEST   3
#define DHCP_ACK       5

/* DHCP option tags */
#define DHCP_OPT_PAD       0
#define DHCP_OPT_SUBNET    1
#define DHCP_OPT_GATEWAY   3
#define DHCP_OPT_DNS       6
#define DHCP_OPT_HOSTNAME  12
#define DHCP_OPT_REQUESTED_IP 50
#define DHCP_OPT_LEASE_TIME   51
#define DHCP_OPT_MSG_TYPE     53
#define DHCP_OPT_SERVER_ID    54
#define DHCP_OPT_PARAM_REQ    55
#define DHCP_OPT_END         255

/* Magic cookie for DHCP options */
#define DHCP_MAGIC_COOKIE 0x63825363

/*
 * DHCP packet structure (RFC 2131) — UDP payload,
 * minimum 236 bytes for option-less BOOTP compatibility.
 */
typedef struct {
    uint8_t  op;             /* 1 = BOOTREQUEST, 2 = BOOTREPLY    */
    uint8_t  htype;          /* Hardware type (1 = Ethernet)     */
    uint8_t  hlen;           /* Hardware address length (6)      */
    uint8_t  hops;           /* Hops (0 for client)              */
    uint32_t xid;            /* Transaction ID                   */
    uint16_t secs;           /* Seconds since DHCP process began */
    uint16_t flags;           /* Flags (0x8000 = broadcast)      */
    uint32_t ciaddr;         /* Client IP (filled on BOUND/ACK)  */
    uint32_t yiaddr;         /* Your IP (assigned by server)     */
    uint32_t siaddr;         /* Server IP (next server to use)   */
    uint32_t giaddr;         /* Relay agent IP                   */
    uint8_t  chaddr[16];     /* Client hardware address (padded) */
    char     sname[64];      /* Server hostname (optional)       */
    char     file[128];      /* Boot filename (optional)         */
    uint32_t magic;          /* 0x63825363                       */
    /* Options follow (variable-length) */
} __attribute__((packed)) dhcp_packet_t;

#define DHCP_BASE_SIZE 236  /* sizeof(dhcp_packet_t) without options */

/* Public API */

/**
 * dhcp_start - Run the DHCP DORA cycle and apply configuration.
 *
 * Broadcasts DISCOVER, waits for OFFER, sends REQUEST, waits for ACK.
 * On success the kernel's OUR_IP, GATEWAY_IP, and DNS_SERVER globals
 * are updated to the DHCP-provided values.
 *
 * @return 0 on success (IP configured), -1 on failure.
 */
int dhcp_start(void);

#endif /* DHCP_H */
