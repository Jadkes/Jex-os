/**
 * @file tcp.c
 * @brief TCP/IP — single-connection client stack.
 *
 * ISR path updates state / RX buffer; shell API blocks polling the NIC.
 * Single core, cli/sti guards on shared state.
 */
#define pr_fmt(fmt) "[TCP] " fmt
#include "kernel/printk.h"
#include "tcp.h"
#include "net.h"
#include "fs.h"
#include "rtl8139.h"
#include "panic.h"
#include "terminal.h"
#include "serial.h"
#include "timer.h"
#include <string.h>

extern void int_to_string(int n, char* str);

/* Connection state — single connection */

static volatile int     tcp_state        = TCP_CLOSED;
static uint32_t         remote_ip        = 0;
static uint16_t         remote_port      = 0;
static uint16_t         local_port       = 0;

static uint32_t         my_seq           = 0;   /* Next seq we'll send   */
static uint32_t         my_ack           = 0;   /* Next ack we expect    */
static uint32_t         peer_isn         = 0;   /* Peer initial seq num  */
static uint32_t         peer_acked_to    = 0;   /* Peer has acked up to here */

/* Last-unacked segment for retransmission */
static uint8_t          tx_buf[TCP_TX_BUF_SIZE];
static uint32_t         tx_len           = 0;

/* Receive ring buffer */
static uint8_t          rx_buf[TCP_RX_BUF_SIZE];
static volatile uint32_t rx_len           = 0;

/* Source MAC of the remote end (for sending replies) */
static uint8_t          peer_mac[6];

static int              retry_count     = 0;

/* Ephemeral port counter */
static uint16_t         next_ephemeral  = 40000;

/* Local helpers */

/* TCP checksum — pseudo-header (RFC 793) then segment,
 * same algorithm as udp_checksum in net.c */
static uint16_t tcp_checksum(uint32_t src, uint32_t dst,
                              const uint16_t* seg, uint32_t seg_len)
{
    tcp_pseudo_t pseudo;
    pseudo.src_ip   = src;
    pseudo.dest_ip  = dst;
    pseudo.zero     = 0;
    pseudo.protocol = IP_PROTO_TCP;
    pseudo.tcp_len  = htons(seg_len);

    static uint8_t buf[sizeof(tcp_pseudo_t) + TCP_TX_BUF_SIZE];
    uint32_t total = sizeof(tcp_pseudo_t) + seg_len;
    if (total > sizeof(buf))
        return 0;

    memcpy(buf, &pseudo, sizeof(tcp_pseudo_t));
    memcpy(buf + sizeof(tcp_pseudo_t), seg, seg_len);

    uint16_t sum = checksum((uint16_t*)buf, total);
    return sum ? sum : 0xFFFF;
}

/*
 * send_tcp_segment - Build Ethernet+IP+TCP segment and transmit.
 * @return 0 on success, -1 if destination MAC cannot be resolved.
 */
static int send_tcp_segment(uint8_t flags, uint32_t seq, uint32_t ack,
                             const uint8_t* payload, uint32_t pay_len)
{
    uint8_t our_mac[6];
    rtl8139_get_mac(our_mac);

    /* Resolve destination MAC (gateway for remote IPs) */
    uint8_t dst_mac[6];
    if (resolve_mac(remote_ip, dst_mac) < 0) {
        net_arp_resolve(remote_ip);
        int timeout = 500;
        while (timeout-- > 0) {
            rtl8139_poll_rx();
            if (resolve_mac(remote_ip, dst_mac) == 0)
                goto mac_ok;
            /* hlt: safe because rtl8139_poll_rx restores IF on exit,
             * so after it returns IF reflects the caller's context
             * (shell: IF=1 → timer ISR fires → ~10 ms granularity).
             * This avoids sleep()'s unconditional sti, which would
             * break if this code were ever called from an ISR. */
            __asm__ volatile("hlt");
        }
        pr_debug("MAC resolution failed\n");
        return -1;
    }
mac_ok:

    /* Save peer MAC for reply path */
    memcpy(peer_mac, dst_mac, 6);

    /* Build TCP header (+ optional MSS for SYN) */
    uint32_t tcp_hdr_len = sizeof(tcp_header_t);
    uint8_t  tcp_options[12] = {0};
    uint32_t opts_len = 0;

    /* SYN segments include the MSS option */
    if (flags & TCP_SYN) {
        tcp_options[0] = TCP_OPT_MSS_KIND;
        tcp_options[1] = TCP_OPT_MSS_LEN;
        tcp_options[2] = (TCP_OPT_MSS_VAL >> 8) & 0xFF;
        tcp_options[3] = TCP_OPT_MSS_VAL & 0xFF;
        opts_len = 4;
    }

    uint32_t data_offset = tcp_hdr_len + opts_len;
    uint32_t tcp_seg_len = data_offset + pay_len;

    /* Static TX buffer avoids 2048 bytes on kernel stack.
     * Single-core with cli/sti guards and non-re-entrant ISR means
     * shell and ISR paths don't overlap; tcp_checksum's static scratch
     * buffer is independently protected with cli/sti below. */
    static uint8_t pkt_buf[PACKET_BUF_SIZE];
    uint8_t* p = pkt_buf;

    p = build_ether_header(p, dst_mac, our_mac, ETHERTYPE_IP);
    p = build_ip_header(p, IP_PROTO_TCP, OUR_IP, remote_ip, tcp_seg_len);

    /* TCP header */
    tcp_header_t* tcp = (tcp_header_t*)p;
    tcp->src_port    = htons(local_port);
    tcp->dest_port   = htons(remote_port);
    tcp->seq_num     = htonl(seq);
    tcp->ack_num     = htonl(ack);
    tcp->data_offset = ((data_offset / 4) << 4);
    tcp->flags       = flags;
    tcp->window      = htons(TCP_RX_BUF_SIZE);
    tcp->checksum    = 0;
    tcp->urgent_ptr  = 0;
    p += tcp_hdr_len;

    /* TCP options */
    if (opts_len > 0) {
        memcpy(p, tcp_options, opts_len);
        p += opts_len;
    }

    if (payload && pay_len > 0) {
        memcpy(p, payload, pay_len);
        p += pay_len;
    }

    /* tcp_checksum uses a static scratch buffer shared between ISR and
     * shell contexts — protect with cli/sti (same pattern as udp_checksum). */
    uint32_t _csum_ef;
    __asm__ volatile("pushf; pop %0" : "=r"(_csum_ef));
    __asm__ volatile("cli");
    tcp->checksum = tcp_checksum(OUR_IP, remote_ip,
                                  (const uint16_t*)tcp, tcp_seg_len);
    if (_csum_ef & 0x200)
        __asm__ volatile("sti");

    uint32_t total = sizeof(eth_header_t) + IP_HEADER_LEN + tcp_seg_len;
    net_send_ether(pkt_buf, total);

    pr_debug("sent flags=0x%x seq=0x%x ack=0x%x len=%u\n",
             flags, seq, ack, pay_len);

    return 0;
}

/* ISR path */

/*
 * handle_tcp - Validate segment against connection tuple, update state/RX buf.
 */
void handle_tcp(uint8_t* data, uint32_t len, uint32_t ip_hdr,
                const uint8_t* src_mac)
{
    ip_header_t* ip  = (ip_header_t*)data;
    uint32_t ip_tot  = ntohs(ip->total_len);

    if (len < ip_hdr + sizeof(tcp_header_t) ||
        ip_tot < ip_hdr + sizeof(tcp_header_t))
        return;

    tcp_header_t* seg = (tcp_header_t*)(data + ip_hdr);
    uint16_t src_port  = ntohs(seg->src_port);
    uint16_t dest_port = ntohs(seg->dest_port);

    /* Only accept segments for our connection */
    if (dest_port != local_port)
        return;
    /* In LISTEN state we accept any source IP/port */
    if (tcp_state != TCP_LISTEN) {
        if (src_port != remote_port && remote_port != 0)
            return;
        if (ip->src_ip != remote_ip && remote_ip != 0)
            return;
    }

    uint32_t seg_data_offset = (seg->data_offset >> 4) * 4;
    uint32_t tcp_payload_len = ip_tot - ip_hdr - seg_data_offset;

    uint32_t seg_seq = ntohl(seg->seq_num);
    uint32_t seg_ack = ntohl(seg->ack_num);

    pr_debug("RX: flags=0x%x seq=0x%x ack=0x%x pay_len=%u\n",
             seg->flags, seg_seq, seg_ack, tcp_payload_len);

    /* ---- Passive open: SYN on LISTEN ---- */
    if (tcp_state == TCP_LISTEN) {
        if ((seg->flags & TCP_SYN) && !(seg->flags & TCP_ACK)) {
            memcpy(peer_mac, src_mac, 6);
            remote_ip   = ip->src_ip;
            remote_port = src_port;
            peer_isn    = seg_seq;

            my_seq = (get_ticks() ^ 0xCAFEBABE);
            my_ack = seg_seq + 1;

            pr_debug("SYN from 0x%x port %u\n", remote_ip, src_port);

            send_tcp_segment(TCP_SYN | TCP_ACK, my_seq, my_ack, NULL, 0);
            my_seq++;

            __asm__ volatile("cli");
            tcp_state = TCP_SYN_RCVD;
            __asm__ volatile("sti");
        }
        return;
    }

    /* ---- Passive open: ACK on SYN_RCVD ---- */
    if (tcp_state == TCP_SYN_RCVD && (seg->flags & TCP_ACK)) {
        if (seg_ack == my_seq) {
            peer_acked_to = seg_ack;
            __asm__ volatile("cli");
            tcp_state = TCP_ESTABLISHED;
            __asm__ volatile("sti");
            pr_debug("connection established (passive open)\n");
        }
        return;
    }

    /* ---- Handle SYN+ACK (reply to our SYN) ---- */
    if (seg->flags & TCP_SYN && seg->flags & TCP_ACK) {
        if (tcp_state == TCP_SYN_SENT && seg_ack == my_seq) {
            peer_isn = seg_seq;
            my_ack   = seg_seq + 1;
            my_seq   = seg_ack;  /* ACK value from peer (byte-order corrected) */
            /* Actually: seg_ack is already in host byte order from ntohl */

            /* Send ACK to complete the three-way handshake.
             * NOTE: pure ACK does NOT consume a sequence number. */
            send_tcp_segment(TCP_ACK, my_seq, my_ack, NULL, 0);

            __asm__ volatile("cli");
            tcp_state = TCP_ESTABLISHED;
            __asm__ volatile("sti");

            pr_debug("connection established\n");
        }
        return;
    }

    /* ---- Handle data + ACK ---- */
    if (seg->flags & TCP_ACK) {
        /* Update what peer has acked (only when advancing) */
        if (seg_ack > peer_acked_to)
            peer_acked_to = seg_ack;
        retry_count = 0;

        /* If we're in FIN_WAIT_1 and peer ACKed our FIN, advance */
        if (tcp_state == TCP_FIN_WAIT_1 && seg_ack >= my_seq) {
            __asm__ volatile("cli");
            tcp_state = TCP_FIN_WAIT_2;
            __asm__ volatile("sti");
            pr_debug("state -> FIN_WAIT_2\n");
        }

        /* LAST_ACK: peer ACKed our FIN → CLOSED */
        if (tcp_state == TCP_LAST_ACK && seg_ack >= my_seq) {
            __asm__ volatile("cli");
            tcp_state = TCP_CLOSED;
            __asm__ volatile("sti");
            pr_debug("state -> CLOSED\n");
        }
    }

    /* Handle incoming data */
    if (tcp_payload_len > 0 && (seg->flags & TCP_PSH || seg->flags & TCP_ACK)) {
        /* Only accept in-order data */
        if (seg_seq == my_ack) {
            uint8_t* tcp_data = data + ip_hdr + seg_data_offset;

            /* Copy to RX buffer (guard from ISR vs shell) */
            __asm__ volatile("cli");
            /* Clamp rx_len to buffer size to avoid underflow in subtraction */
            if (rx_len > TCP_RX_BUF_SIZE)
                rx_len = TCP_RX_BUF_SIZE;
            uint32_t copy = tcp_payload_len;
            if (rx_len + copy > TCP_RX_BUF_SIZE || rx_len + copy < rx_len)
                copy = TCP_RX_BUF_SIZE - rx_len;
            memcpy(rx_buf + rx_len, tcp_data, copy);
            rx_len += copy;
            __asm__ volatile("sti");

            my_ack += tcp_payload_len;

            /* Send ACK for received data */
            send_tcp_segment(TCP_ACK, my_seq, my_ack, NULL, 0);
            pr_debug("received %u bytes of data\n", tcp_payload_len);
        } else {
            pr_debug("out-of-order data (got seq=0x%x expected seq=0x%x), sending dup ACK\n",
                     seg_seq, my_ack);
            /* Send duplicate ACK */
            send_tcp_segment(TCP_ACK, my_seq, my_ack, NULL, 0);
        }
    }

    /* ---- Handle FIN ---- */
    if (seg->flags & TCP_FIN) {
        my_ack = seg_seq + tcp_payload_len + 1;

        pr_debug("received FIN\n");

        if (tcp_state == TCP_ESTABLISHED) {
            /* Peer is closing first (passive close) */
            send_tcp_segment(TCP_ACK | TCP_FIN, my_seq, my_ack, NULL, 0);
            my_seq++;
            __asm__ volatile("cli");
            tcp_state = TCP_LAST_ACK;
            __asm__ volatile("sti");
        } else if (tcp_state == TCP_FIN_WAIT_1 || tcp_state == TCP_FIN_WAIT_2) {
            send_tcp_segment(TCP_ACK, my_seq, my_ack, NULL, 0);
            __asm__ volatile("cli");
            tcp_state = TCP_TIME_WAIT;
            __asm__ volatile("sti");
        }
    }

    /* ---- Handle RST ---- */
    if (seg->flags & TCP_RST) {
        pr_debug("received RST\n");
        __asm__ volatile("cli");
        tcp_state = TCP_CLOSED;
        __asm__ volatile("sti");
    }
}

/* Public API — blocking (shell context) */

/*
 * tcp_connect - Open a TCP connection to a remote host.
 *
 * Resolves the destination MAC (via gateway if needed), picks a
 * local ephemeral port, sends SYN, and busy-waits for SYN+ACK.
 */
int tcp_connect(uint32_t dest_ip, uint16_t dest_port, uint16_t src_port)
{
    if (tcp_state != TCP_CLOSED) {
        pr_debug("already connected, aborting first\n");
        tcp_abort();
        /* Wait for state to settle */
        int timeout = 200;
        while (timeout-- > 0 && tcp_state != TCP_CLOSED) {
            rtl8139_poll_rx();
            sleep(10);
        }
    }

    /* Assign connection parameters */
    remote_ip   = dest_ip;
    remote_port = dest_port;

    if (src_port == 0)
        local_port = next_ephemeral++;
    else
        local_port = src_port;

    /* Pick ISN — use timer for some randomness */
    my_seq   = get_ticks() ^ 0xDEADBEEF;
    my_ack   = 0;
    peer_isn = 0;
    peer_acked_to = my_seq;
    rx_len   = 0;
    tx_len   = 0;
    retry_count = 0;

    /* Build and send SYN */
    __asm__ volatile("cli");
    tcp_state = TCP_SYN_SENT;
    __asm__ volatile("sti");

    pr_debug("connecting to 0x%x port %u (local port %u)\n",
             dest_ip, dest_port, local_port);

    int ret = send_tcp_segment(TCP_SYN, my_seq, 0, NULL, 0);
    if (ret < 0) {
        tcp_state = TCP_CLOSED;
        return -1;
    }
    my_seq++;   /* SYN consumes a sequence number */

    /* Busy-wait for SYN+ACK */
    int attempts = 0;
    while (attempts < TCP_MAX_RETRIES) {
        uint32_t deadline = get_ticks() + TCP_RETRY_TIMEOUT_MS / 10;

        while (get_ticks() < deadline) {
            rtl8139_poll_rx();
            if (tcp_state == TCP_ESTABLISHED) {
                pr_debug("connected successfully\n");
                return 0;
            }
            if (tcp_state == TCP_CLOSED) {
                pr_debug("connection rejected (RST)\n");
                return -1;
            }
            /* Small sleep to avoid busy-spinning too hard */
            __asm__ volatile("pause");
        }

        /* Retransmit SYN on timeout */
        attempts++;
        pr_debug("SYN timeout, retransmitting (attempt %d/%d)\n",
                 attempts, TCP_MAX_RETRIES);

        send_tcp_segment(TCP_SYN, my_seq - 1, 0, NULL, 0);
    }

    pr_debug("connection timeout\n");
    tcp_state = TCP_CLOSED;
    return -1;
}

/*
 * tcp_send - Send data and wait for acknowledgment.
 *
 * Copies the data, sends it, then busy-waits until the data has been
 * acknowledged or the retry budget is exhausted.
 */
int tcp_send(const uint8_t* data, uint32_t len)
{
    if (tcp_state != TCP_ESTABLISHED) {
        pr_debug("not connected\n");
        return -1;
    }

    uint32_t sent = 0;

    while (sent < len) {
        /* Segment size limited to avoid exceeding the TX buffer */
        uint32_t chunk = len - sent;
        if (chunk > 1024)
            chunk = 1024;

        uint32_t seg_seq = my_seq;

        /* Copy to TX buffer for retransmission */
        memcpy(tx_buf, data + sent, chunk);
        tx_len       = chunk;

        int ret = send_tcp_segment(TCP_PSH | TCP_ACK, seg_seq, my_ack,
                                    data + sent, chunk);
        if (ret < 0) {
            pr_debug("send failed (MAC)\n");
            return (int)sent > 0 ? (int)sent : -1;
        }

        my_seq += chunk;

        /* Busy-wait for ACK */
        int attempts = 0;
        while (attempts < TCP_MAX_RETRIES) {
            uint32_t deadline = get_ticks() + TCP_RETRY_TIMEOUT_MS / 10;

            while (get_ticks() < deadline) {
                rtl8139_poll_rx();
                if (peer_acked_to >= seg_seq + chunk) {
                    /* This chunk fully acknowledged */
                    goto chunk_acked;
                }
                if (tcp_state == TCP_CLOSED) {
                    pr_debug("connection lost during send\n");
                    return (int)sent > 0 ? (int)sent : -1;
                }
                __asm__ volatile("pause");
            }

            /* Retransmit */
            attempts++;
            pr_debug("data timeout, retransmitting\n");
            send_tcp_segment(TCP_PSH | TCP_ACK, seg_seq, my_ack,
                              tx_buf, tx_len);
        }

        pr_debug("send timeout after %d retries\n", attempts);
        return (int)sent;

    chunk_acked:
        sent += chunk;
    }

    return (int)sent;
}

/*
 * tcp_receive - Copy received data into a user buffer.
 */
int tcp_receive(uint8_t* buf, uint32_t buf_len)
{
    if (rx_len == 0)
        return 0;

    uint32_t copy = rx_len;
    if (copy > buf_len)
        copy = buf_len;

    /* Guard: prevent data from being written by ISR while we read */
    __asm__ volatile("cli");
    memcpy(buf, rx_buf, copy);
    /* Shift remaining data (memmove not available in freestanding env) */
    uint32_t rest = rx_len - copy;
    if (rest > 0) {
        for (uint32_t i = 0; i < rest; i++)
            rx_buf[i] = rx_buf[i + copy];
    }
    rx_len = rest;
    __asm__ volatile("sti");

    return (int)copy;
}

/*
 * tcp_available - Return bytes in the RX buffer.
 */
int tcp_available(void)
{
    return (int)rx_len;
}

/*
 * tcp_close - Graceful close (send FIN, wait for FIN+ACK).
 */
void tcp_close(void)
{
    /* Already in LAST_ACK (server closed first) — just wait for CLOSED */
    if (tcp_state == TCP_LAST_ACK) {
        pr_debug("waiting for LAST_ACK -> CLOSED\n");
        int timeout = 200; /* ~2 seconds */
        while (timeout-- > 0) {
            rtl8139_poll_rx();
            if (tcp_state == TCP_CLOSED) {
                pr_debug("connection closed\n");
                return;
            }
            sleep(10);
        }
        pr_debug("LAST_ACK timeout, aborting\n");
        tcp_abort();
        return;
    }

    if (tcp_state != TCP_ESTABLISHED && tcp_state != TCP_CLOSE_WAIT) {
        pr_debug("nothing to close (state=0x%x)\n", tcp_state);
        return;
    }

    pr_debug("closing connection\n");

    /* Send FIN */
    __asm__ volatile("cli");
    tcp_state = TCP_FIN_WAIT_1;
    __asm__ volatile("sti");

    send_tcp_segment(TCP_FIN | TCP_ACK, my_seq, my_ack, NULL, 0);
    my_seq++;

    /* Wait for FIN+ACK or TIME_WAIT */
    int attempts = 0;
    while (attempts < TCP_MAX_RETRIES) {
        uint32_t deadline = get_ticks() + TCP_RETRY_TIMEOUT_MS / 10;

        while (get_ticks() < deadline) {
            rtl8139_poll_rx();
            if (tcp_state == TCP_TIME_WAIT || tcp_state == TCP_CLOSED ||
                tcp_state == TCP_LAST_ACK) {
                /* Connection closed */
                __asm__ volatile("cli");
                tcp_state = TCP_CLOSED;
                __asm__ volatile("sti");
                pr_debug("connection closed\n");
                return;
            }
            if (tcp_state == TCP_FIN_WAIT_2)
                break;  /* Peer ACKed our FIN, waiting for their FIN */
            __asm__ volatile("pause");
        }

        if (tcp_state == TCP_FIN_WAIT_2) {
            /* Wait for peer's FIN */
            uint32_t fin_deadline = get_ticks() + 200;  /* 2 seconds */
            while (get_ticks() < fin_deadline) {
                rtl8139_poll_rx();
                if (tcp_state == TCP_TIME_WAIT || tcp_state == TCP_CLOSED) {
                    __asm__ volatile("cli");
                    tcp_state = TCP_CLOSED;
                    __asm__ volatile("sti");
                    pr_debug("connection closed\n");
                    return;
                }
                __asm__ volatile("pause");
            }
            /* Timeout waiting for peer's FIN — force close */
            pr_debug("peer FIN timeout, aborting\n");
            tcp_abort();
            return;
        }

        /* Retransmit FIN */
        attempts++;
        pr_debug("FIN timeout, retransmitting\n");
        send_tcp_segment(TCP_FIN | TCP_ACK, my_seq - 1, my_ack, NULL, 0);
    }

    /* Force close after all retries exhausted */
    pr_debug("close timeout, aborting\n");
    tcp_abort();
}

/*
 * tcp_abort - Send RST to force-close the connection.
 */
void tcp_abort(void)
{
    if (tcp_state == TCP_CLOSED)
        return;

    pr_debug("sending RST\n");
    send_tcp_segment(TCP_RST | TCP_ACK, my_seq, my_ack, NULL, 0);

    __asm__ volatile("cli");
    tcp_state = TCP_CLOSED;
    __asm__ volatile("sti");
}

/* State accessors */

int tcp_get_state(void)     { return (int)tcp_state; }
uint32_t tcp_get_remote_ip(void)   { return remote_ip; }
uint16_t tcp_get_remote_port(void) { return remote_port; }

/*
 * tcp_listen - Start listening on a port (passive open).
 *
 * Sets state to TCP_LISTEN and clears any previous connection state.
 * Call tcp_accept() to block until a connection arrives.
 */
int tcp_listen(uint16_t port)
{
    if (tcp_state != TCP_CLOSED) {
        pr_debug("already active (state=%d), aborting\n", tcp_state);
        tcp_abort();
        int timeout = 200;
        while (timeout-- > 0 && tcp_state != TCP_CLOSED) {
            rtl8139_poll_rx();
            sleep(10);
        }
    }

    remote_ip   = 0;
    remote_port = 0;
    local_port  = port;
    rx_len      = 0;
    tx_len      = 0;

    __asm__ volatile("cli");
    tcp_state = TCP_LISTEN;
    __asm__ volatile("sti");

    pr_debug("listening on port %u\n", port);
    return 0;
}

/*
 * tcp_accept - Accept a pending connection (busy-wait).
 *
 * Blocks polling the NIC until the three-way handshake completes
 * (LISTEN → SYN_RCVD → ESTABLISHED). Returns 0 on success.
 */
int tcp_accept(void)
{
    if (tcp_state != TCP_LISTEN) {
        pr_debug("not listening (state=%d)\n", tcp_state);
        return -1;
    }

    pr_debug("waiting for connection...\n");

    /* Busy-wait up to ~20 seconds (2000 × 10ms sleep) */
    int timeout = 2000;
    while (timeout-- > 0) {
        rtl8139_poll_rx();
        if (tcp_state == TCP_ESTABLISHED) {
            pr_debug("connection accepted from port %u\n", remote_port);
            return 0;
        }
        if (tcp_state == TCP_CLOSED) {
            pr_debug("connection aborted during accept\n");
            return -1;
        }
        sleep(10);
    }

    pr_debug("accept timeout\n");
    tcp_abort();
    return -1;
}

/* HTTP GET convenience */

/*
 * http_get - DNS-resolve hostname, TCP connect, send HTTP/1.0 GET, read response, close.
 */
int http_get(const char* hostname, uint16_t port, const char* path)
{
    char ip_buf[16];

    /* ---- Resolve hostname ---- */
    pr_debug("resolving %s\n", hostname);
    terminal_writestring("Resolving ");
    terminal_writestring(hostname);
    terminal_writestring("...\n");

    uint32_t ip = net_dns_resolve(hostname);
    if (ip == 0) {
        pr_debug("DNS resolution failed\n");
        terminal_writestring("DNS resolution failed\n");
        return -1;
    }

    /* Print resolved IP */
    pr_debug("resolved to 0x%x\n", ip);
    uint8_t* ipb = (uint8_t*)&ip;
    int_to_string(ipb[0], ip_buf); terminal_writestring(ip_buf); terminal_putchar('.');
    int_to_string(ipb[1], ip_buf); terminal_writestring(ip_buf); terminal_putchar('.');
    int_to_string(ipb[2], ip_buf); terminal_writestring(ip_buf); terminal_putchar('.');
    int_to_string(ipb[3], ip_buf); terminal_writestring(ip_buf);
    terminal_writestring("\n");

    /* ---- TCP Connect ---- */
    pr_debug("connecting...\n");
    terminal_writestring("Connecting...\n");
    if (tcp_connect(ip, port, 0) < 0) {
        terminal_writestring("Connection failed\n");
        return -1;
    }

    /* ---- Build HTTP request (separate buffer from tx_buf) ---- */
    static char http_req_buf[TCP_TX_BUF_SIZE];
    uint32_t req_len = 0;
    char* req = http_req_buf;

    /* "GET /path HTTP/1.0\r\n" */
    {
        const char* s = "GET ";
        while (*s) req[req_len++] = *s++;
        s = path;
        while (*s) req[req_len++] = *s++;
        s = " HTTP/1.0\r\n";
        while (*s) req[req_len++] = *s++;
    }
    /* "Host: hostname\r\n" */
    {
        const char* s = "Host: ";
        while (*s) req[req_len++] = *s++;
        s = hostname;
        while (*s) req[req_len++] = *s++;
        s = "\r\n";
        while (*s) req[req_len++] = *s++;
    }
    /* "Connection: close\r\n" */
    {
        const char* s = "Connection: close\r\n";
        while (*s) req[req_len++] = *s++;
    }
    /* Final \r\n */
    req[req_len++] = '\r';
    req[req_len++] = '\n';

    pr_debug("request len=%u\n", req_len);

    /* ---- Send request ---- */
    pr_debug("sending request...\n");
    terminal_writestring("Sending request...\n");
    int sent = tcp_send((const uint8_t*)req, req_len);
    if (sent < 0) {
        terminal_writestring("Send failed\n");
        tcp_abort();
        return -1;
    }

    /* ---- Read response ---- */
    terminal_writestring("Response:\n");
    {
        char resp_buf[1024];
        int total_read = 0;
        int timeout = 500;  /* ~5 seconds at 100Hz */

        while (timeout-- > 0) {
            rtl8139_poll_rx();

            int avail = tcp_available();
            while (avail > 0) {
                uint32_t to_read = (uint32_t)avail;
                if (to_read > sizeof(resp_buf) - 1)
                    to_read = sizeof(resp_buf) - 1;

                int n = tcp_receive((uint8_t*)resp_buf, to_read);
                if (n > 0) {
                    resp_buf[n] = '\0';
                    terminal_writestring(resp_buf);
                    pr_debug("received %d bytes\n", n);
                    total_read += n;
                    timeout = 500;  /* Reset timeout on received data */
                }
                avail = tcp_available();
            }

            if (tcp_state == TCP_CLOSED || tcp_state == TCP_TIME_WAIT)
                break;

            sleep(10);
        }

        if (total_read == 0) {
            terminal_writestring("(no response data)\n");
            pr_debug("no response received\n");
        } else {
            pr_debug("total received %d bytes\n", total_read);
        }
    }

    /* ---- Close ---- */
    tcp_close();
    return 0;
}

/*
 * http_serve - Listen, accept, and serve one HTTP request.
 *
 * Listens on @port, accepts one connection via the passive-open TCP
 * path (LISTEN → SYN_RCVD → ESTABLISHED), parses the HTTP GET request
 * path, and serves the file from JexFS. Falls back to a default
 * "Hello from JexOS" page. Handles one request and closes.
 */
int http_serve(uint16_t port)
{
    char num_buf[8];

    /* ---- Listen ---- */
    if (tcp_listen(port) < 0) {
        terminal_writestring("Listen failed\n");
        return -1;
    }

    terminal_writestring("Listening on port ");
    int_to_string(port, num_buf);
    terminal_writestring(num_buf);
    terminal_writestring("...\n");

    /* ---- Accept ---- */
    if (tcp_accept() < 0) {
        terminal_writestring("Accept failed\n");
        return -1;
    }

    terminal_writestring("Connection from ");
    {
        uint8_t* ipb = (uint8_t*)&remote_ip;
        int_to_string(ipb[0], num_buf); terminal_writestring(num_buf); terminal_putchar('.');
        int_to_string(ipb[1], num_buf); terminal_writestring(num_buf); terminal_putchar('.');
        int_to_string(ipb[2], num_buf); terminal_writestring(num_buf); terminal_putchar('.');
        int_to_string(ipb[3], num_buf); terminal_writestring(num_buf);
    }
    terminal_writestring("\n");
    terminal_writestring("Receiving request...\n");

    /* ---- Read HTTP request ---- */
    static char req_buf[1024];
    int req_len = 0;
    int timeout = 1000; /* ~10 seconds */

    while (timeout-- > 0 && req_len < (int)sizeof(req_buf) - 1) {
        rtl8139_poll_rx();
        if (tcp_state != TCP_ESTABLISHED)
            break;
        int avail = tcp_available();
        if (avail > 0) {
            int n = tcp_receive((uint8_t*)req_buf + req_len,
                                (uint32_t)(sizeof(req_buf) - 1 - req_len));
            if (n > 0) {
                req_len += n;
                timeout = 1000;
                /* Stop at \r\n\r\n (end of HTTP headers) */
                if (req_len >= 4 &&
                    req_buf[req_len - 4] == '\r' &&
                    req_buf[req_len - 3] == '\n' &&
                    req_buf[req_len - 2] == '\r' &&
                    req_buf[req_len - 1] == '\n')
                    break;
            }
        }
        __asm__ volatile("pause");
    }

    if (req_len == 0) {
        terminal_writestring("No request received\n");
        tcp_close();
        return -1;
    }

    req_buf[req_len] = '\0';
    pr_debug("HTTP request (%d bytes)\n", req_len);

    /* ---- Parse GET path ---- */
    char path[256];
    int path_len = 0;

    if (req_len < 5 || req_buf[0] != 'G' || req_buf[1] != 'E' ||
        req_buf[2] != 'T' || req_buf[3] != ' ') {
        pr_debug("not a GET request\n");
        {
            const char* resp = "HTTP/1.0 400 Bad Request\r\nContent-Length: 15\r\n\r\n400 Bad Request\n";
            tcp_send((const uint8_t*)resp, strlen(resp));
        }
        tcp_close();
        return -1;
    }

    int gi = 4; /* skip "GET " */
    while (gi < req_len && req_buf[gi] != ' ' && req_buf[gi] != '\r' &&
           path_len < (int)sizeof(path) - 1)
        path[path_len++] = req_buf[gi++];
    path[path_len] = '\0';

    pr_debug("GET %s\n", path);

    /* ---- Try to serve file from JexFS ---- */
    static char resp_buf[4096];
    uint32_t body_len = 0;
    const char* content_type = "text/html";

    /* "/" -> "/index.html" */
    if (path_len == 1 && path[0] == '/') {
        path[0] = '/';
        memcpy(path + 1, "index.html", 11); /* includes NUL */
    }

    int fd = fs_open(path, 0); /* O_RDONLY = 0 */
    if (fd >= 0) {
        uint32_t file_size = (uint32_t)fs_seek(fd, 0, 2); /* SEEK_END */
        fs_seek(fd, 0, 0); /* SEEK_SET */

        if (file_size <= sizeof(resp_buf)) {
            int nread = fs_read(fd, resp_buf, file_size);
            if (nread > 0)
                body_len = (uint32_t)nread;
        } else {
            pr_debug("file too large: %u\n", file_size);
        }
        fs_close(fd);
    }

    if (body_len == 0) {
        /* File not found or too large — serve default page */
        const char* default_page =
            "<!DOCTYPE html>\n"
            "<html><head><title>JexOS</title></head><body>\n"
            "<h1>Hello from JexOS!</h1>\n"
            "<p>Your hobby OS is serving HTTP.</p>\n"
            "</body></html>\n";
        uint32_t dflen = strlen(default_page);
        if (dflen <= sizeof(resp_buf)) {
            memcpy(resp_buf, default_page, dflen);
            body_len = dflen;
        }
    }

    /* ---- Build HTTP response header ---- */
    char header[256];
    uint32_t hdr_len = 0;

    {
        const char* s = "HTTP/1.0 200 OK\r\nContent-Type: ";
        while (*s) header[hdr_len++] = *s++;
        s = content_type;
        while (*s) header[hdr_len++] = *s++;
        s = "\r\nContent-Length: ";
        while (*s) header[hdr_len++] = *s++;
        int_to_string((int)body_len, num_buf);
        {
            char* np = num_buf;
            while (*np) header[hdr_len++] = *np++;
        }
        header[hdr_len++] = '\r';
        header[hdr_len++] = '\n';
        header[hdr_len++] = '\r';
        header[hdr_len++] = '\n';
    }

    tcp_send((const uint8_t*)header, hdr_len);
    tcp_send((const uint8_t*)resp_buf, body_len);

    terminal_writestring("200 OK (");
    int_to_string((int)body_len, num_buf);
    terminal_writestring(num_buf);
    terminal_writestring(" bytes)\n");
    pr_debug("served %u bytes\n", body_len);

    /* ---- Close ---- */
    tcp_close();
    return 0;
}
