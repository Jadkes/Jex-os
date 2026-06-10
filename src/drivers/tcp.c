/**
 * @file tcp.c
 * @brief TCP/IP — single-connection client stack.
 *
 * Purpose: Minimal TCP client for outbound connections (HTTP GET).
 *          Implements a blocking synchronous API (tcp_connect, tcp_send,
 *          tcp_close) with timer-based retransmission.
 *
 * Design:
 *   - Single connection only (multiple sequential connections OK).
 *   - State machine: CLOSED → SYN_SENT → ESTABLISHED → FIN_WAIT_1 →
 *                    FIN_WAIT_2 → TIME_WAIT → CLOSED
 *   - The ISR path (handle_tcp) updates connection state and copies
 *     received data into the RX buffer.
 *   - The shell-context API busy-waits, polling the NIC via
 *     rtl8139_poll_rx(), until the desired state transition completes.
 *
 * Thread-safety: cli/sti guards on shared state (seq/ack nums, state).
 *                Single-core means no true concurrency — the guards
 *                prevent the ISR from seeing torn writes.
 *
 * Dependencies: net.h (build_ether_header, build_ip_header, checksum,
 *               resolve_mac, OUR_IP, net_send_ether, GATEWAY_IP),
 *               rtl8139.h (rtl8139_poll_rx), panic.h (format_hex).
 */

#include "tcp.h"
#include "net.h"
#include "rtl8139.h"
#include "panic.h"
#include "terminal.h"
#include "serial.h"
#include "timer.h"
#include <string.h>

/* Forward: int_to_string for serial/terminal output */
extern void int_to_string(int n, char* str);

/* ----------------------------------------------------------------- */
/*  Connection State (static — single connection)                    */
/* ----------------------------------------------------------------- */

static volatile int     tcp_state        = TCP_CLOSED;
static uint32_t         local_ip         = 0;
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
static uint32_t         tx_marker        = 0;   /* Index where unsent data begins */
static uint32_t         tx_retry_marker  = 0;   /* Where retransmit from (seq) */

/* Receive ring buffer */
static uint8_t          rx_buf[TCP_RX_BUF_SIZE];
static volatile uint32_t rx_len           = 0;
static uint32_t         rx_bytes_consumed = 0;   /* Bytes already read by app */

/* Source MAC of the remote end (for sending replies) */
static uint8_t          peer_mac[6];

/* Connection timeout tracking */
static uint32_t         last_send_tick  = 0;
static int              retry_count     = 0;

/* Ephemeral port counter */
static uint16_t         next_ephemeral  = 40000;

/* ----------------------------------------------------------------- */
/*  Local helpers                                                    */
/* ----------------------------------------------------------------- */

/*
 * tcp_checksum - Compute TCP checksum with pseudo-header.
 *
 * Same pattern as udp_checksum in net.c — prepend the 12-byte
 * pseudo-header (RFC 793) and checksum the combined block.
 * Returns 0 if the result is 0 (which RFC says to send as 0xFFFF).
 */
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
 * send_tcp_segment - Build and transmit a raw TCP segment.
 *
 * Constructs Ethernet + IP + TCP headers, possibly with payload,
 * computes checksums, and sends via net_send_ether().
 *
 * @param flags    TCP flags (SYN, ACK, FIN, PSH, etc).
 * @param seq      Sequence number.
 * @param ack      Acknowledgment number.
 * @param payload  Optional payload data (NULL if none).
 * @param pay_len  Length of payload (0 if none).
 * @return         0 on success, -1 on MAC resolution failure.
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
            sleep(10);
        }
        log_serial("TCP: MAC resolution failed\n");
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

    /* Use a local buffer for packet construction (send_buf is static in net.c) */
    uint8_t pkt_buf[PACKET_BUF_SIZE];
    uint8_t* p = pkt_buf;

    p = build_ether_header(p, dst_mac, our_mac, ETHERTYPE_IP);
    p = build_ip_header(p, IP_PROTO_TCP, OUR_IP, remote_ip, tcp_seg_len);

    /* TCP header */

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

    /* Payload */
    if (payload && pay_len > 0) {
        memcpy(p, payload, pay_len);
        p += pay_len;
    }

    /* Compute TCP checksum (includes pseudo-header) */
    tcp->checksum = tcp_checksum(OUR_IP, remote_ip,
                                  (const uint16_t*)tcp, tcp_seg_len);

    uint32_t total = sizeof(eth_header_t) + IP_HEADER_LEN + tcp_seg_len;
    net_send_ether(pkt_buf, total);

    log_serial("TCP: sent flags=0x");
    log_hex_serial(flags);
    log_serial(" seq=0x");
    log_hex_serial(seq);
    log_serial(" ack=0x");
    log_hex_serial(ack);
    log_serial(" len=");
    log_hex_serial(pay_len);
    log_serial("\n");

    return 0;
}

/* ----------------------------------------------------------------- */
/*  ISR path: called from handle_ip() when IP_PROTO_TCP              */
/* ----------------------------------------------------------------- */

/*
 * handle_tcp - Process an incoming TCP segment.
 *
 * Called from the ISR context via net_process_packet → handle_ip.
 * Validates the segment against our connection tuple and updates
 * the state machine / RX buffer accordingly.
 *
 * @param data    Pointer to the IP header (entire datagram).
 * @param len     Total datagram length.
 * @param ip_hdr  IP header length in bytes.
 * @param src_mac Source MAC from the Ethernet header.
 */
void handle_tcp(uint8_t* data, uint32_t len, uint32_t ip_hdr,
                const uint8_t* src_mac)
{
    ip_header_t* ip  = (ip_header_t*)data;
    uint32_t ip_tot  = ntohs(ip->total_len);

    if (len < ip_hdr + sizeof(tcp_header_t))
        return;
    if (ip_tot < ip_hdr + sizeof(tcp_header_t))
        return;

    tcp_header_t* seg = (tcp_header_t*)(data + ip_hdr);
    uint16_t src_port  = ntohs(seg->src_port);
    uint16_t dest_port = ntohs(seg->dest_port);

    /* Only accept segments for our connection */
    if (dest_port != local_port)
        return;
    if (src_port != remote_port && remote_port != 0)
        return;
    if (ip->src_ip != remote_ip && remote_ip != 0)
        return;

    uint32_t seg_data_offset = (seg->data_offset >> 4) * 4;
    uint32_t tcp_payload_len = ip_tot - ip_hdr - seg_data_offset;

    uint32_t seg_seq = ntohl(seg->seq_num);
    uint32_t seg_ack = ntohl(seg->ack_num);

    log_serial("TCP_RX: flags=0x");
    log_hex_serial(seg->flags);
    log_serial(" seq=0x");
    log_hex_serial(seg_seq);
    log_serial(" ack=0x");
    log_hex_serial(seg_ack);
    log_serial(" pay_len=");
    log_hex_serial(tcp_payload_len);
    log_serial("\n");

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

            log_serial("TCP: connection established\n");
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
            log_serial("TCP: state -> FIN_WAIT_2\n");
        }

        /* LAST_ACK: peer ACKed our FIN → CLOSED */
        if (tcp_state == TCP_LAST_ACK && seg_ack >= my_seq) {
            __asm__ volatile("cli");
            tcp_state = TCP_CLOSED;
            __asm__ volatile("sti");
            log_serial("TCP: state -> CLOSED\n");
        }
    }

    /* Handle incoming data */
    if (tcp_payload_len > 0 && (seg->flags & TCP_PSH || seg->flags & TCP_ACK)) {
        /* Only accept in-order data */
        if (seg_seq == my_ack) {
            uint8_t* tcp_data = data + ip_hdr + seg_data_offset;

            /* Copy to RX buffer (guard from ISR vs shell) */
            __asm__ volatile("cli");
            uint32_t copy = tcp_payload_len;
            if (rx_len + copy > TCP_RX_BUF_SIZE)
                copy = TCP_RX_BUF_SIZE - rx_len;
            memcpy(rx_buf + rx_len, tcp_data, copy);
            rx_len += copy;
            __asm__ volatile("sti");

            my_ack += tcp_payload_len;

            /* Send ACK for received data */
            send_tcp_segment(TCP_ACK, my_seq, my_ack, NULL, 0);
            log_serial("TCP: received ");
            log_hex_serial(tcp_payload_len);
            log_serial(" bytes of data\n");
        } else {
            log_serial("TCP: out-of-order data (got seq=");
            log_hex_serial(seg_seq);
            log_serial(" expected seq=");
            log_hex_serial(my_ack);
            log_serial("), sending dup ACK\n");
            /* Send duplicate ACK */
            send_tcp_segment(TCP_ACK, my_seq, my_ack, NULL, 0);
        }
    }

    /* ---- Handle FIN ---- */
    if (seg->flags & TCP_FIN) {
        my_ack = seg_seq + tcp_payload_len + 1;

        log_serial("TCP: received FIN\n");

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
        log_serial("TCP: received RST\n");
        __asm__ volatile("cli");
        tcp_state = TCP_CLOSED;
        __asm__ volatile("sti");
    }
}

/* ----------------------------------------------------------------- */
/*  Public API — blocking (shell context)                            */
/* ----------------------------------------------------------------- */

/*
 * tcp_connect - Open a TCP connection to a remote host.
 *
 * Resolves the destination MAC (via gateway if needed), picks a
 * local ephemeral port, sends SYN, and busy-waits for SYN+ACK.
 */
int tcp_connect(uint32_t dest_ip, uint16_t dest_port, uint16_t src_port)
{
    if (tcp_state != TCP_CLOSED) {
        log_serial("TCP: already connected, aborting first\n");
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

    log_serial("TCP: connecting to ");
    log_hex_serial(dest_ip);
    log_serial(" port ");
    log_hex_serial(dest_port);
    log_serial(" (local port ");
    log_hex_serial(local_port);
    log_serial(")\n");

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
                log_serial("TCP: connected successfully\n");
                return 0;
            }
            if (tcp_state == TCP_CLOSED) {
                log_serial("TCP: connection rejected (RST)\n");
                return -1;
            }
            /* Small sleep to avoid busy-spinning too hard */
            __asm__ volatile("pause");
        }

        /* Retransmit SYN on timeout */
        attempts++;
        log_serial("TCP: SYN timeout, retransmitting (attempt ");
        char buf[4];
        int_to_string(attempts, buf);
        log_serial(buf);
        log_serial("/");
        int_to_string(TCP_MAX_RETRIES, buf);
        log_serial(buf);
        log_serial(")\n");

        send_tcp_segment(TCP_SYN, my_seq - 1, 0, NULL, 0);
    }

    log_serial("TCP: connection timeout\n");
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
        log_serial("TCP: not connected\n");
        return -1;
    }

    uint32_t remaining = len;
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
        tx_marker    = sent;
        tx_retry_marker = seg_seq;

        int ret = send_tcp_segment(TCP_PSH | TCP_ACK, seg_seq, my_ack,
                                    data + sent, chunk);
        if (ret < 0) {
            log_serial("TCP: send failed (MAC)\n");
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
                    log_serial("TCP: connection lost during send\n");
                    return (int)sent > 0 ? (int)sent : -1;
                }
                __asm__ volatile("pause");
            }

            /* Retransmit */
            attempts++;
            log_serial("TCP: data timeout, retransmitting\n");
            send_tcp_segment(TCP_PSH | TCP_ACK, seg_seq, my_ack,
                              tx_buf, tx_len);
        }

        log_serial("TCP: send timeout after ");
        char buf[4];
        int_to_string(attempts, buf);
        log_serial(buf);
        log_serial(" retries\n");
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
        log_serial("TCP: waiting for LAST_ACK -> CLOSED\n");
        int timeout = 200; /* ~2 seconds */
        while (timeout-- > 0) {
            rtl8139_poll_rx();
            if (tcp_state == TCP_CLOSED) {
                log_serial("TCP: connection closed\n");
                return;
            }
            sleep(10);
        }
        log_serial("TCP: LAST_ACK timeout, aborting\n");
        tcp_abort();
        return;
    }

    if (tcp_state != TCP_ESTABLISHED && tcp_state != TCP_CLOSE_WAIT) {
        log_serial("TCP: nothing to close (state=");
        log_hex_serial(tcp_state);
        log_serial(")\n");
        return;
    }

    log_serial("TCP: closing connection\n");

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
                log_serial("TCP: connection closed\n");
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
                    log_serial("TCP: connection closed\n");
                    return;
                }
                __asm__ volatile("pause");
            }
            /* Timeout waiting for peer's FIN — force close */
            log_serial("TCP: peer FIN timeout, aborting\n");
            tcp_abort();
            return;
        }

        /* Retransmit FIN */
        attempts++;
        log_serial("TCP: FIN timeout, retransmitting\n");
        send_tcp_segment(TCP_FIN | TCP_ACK, my_seq - 1, my_ack, NULL, 0);
    }

    /* Force close after all retries exhausted */
    log_serial("TCP: close timeout, aborting\n");
    tcp_abort();
}

/*
 * tcp_abort - Send RST to force-close the connection.
 */
void tcp_abort(void)
{
    if (tcp_state == TCP_CLOSED)
        return;

    log_serial("TCP: sending RST\n");
    send_tcp_segment(TCP_RST | TCP_ACK, my_seq, my_ack, NULL, 0);

    __asm__ volatile("cli");
    tcp_state = TCP_CLOSED;
    __asm__ volatile("sti");
}

/* ----------------------------------------------------------------- */
/*  State accessors                                                  */
/* ----------------------------------------------------------------- */

int tcp_get_state(void)
{
    return (int)tcp_state;
}

uint32_t tcp_get_remote_ip(void)
{
    return remote_ip;
}

uint16_t tcp_get_remote_port(void)
{
    return remote_port;
}

/* ----------------------------------------------------------------- */
/*  HTTP GET Convenience                                             */
/* ----------------------------------------------------------------- */

/*
 * http_get - Perform an HTTP GET request and print the response.
 *
 * 1. DNS-resolve the hostname.
 * 2. TCP-connect to host:port.
 * 3. Send "GET /path HTTP/1.0\r\nHost: host\r\nConnection: close\r\n\r\n".
 * 4. Read response data and print it to the terminal.
 * 5. TCP-close.
 */
int http_get(const char* hostname, uint16_t port, const char* path)
{
    char buf[128];
    char ip_buf[16];

    /* ---- Resolve hostname ---- */
    log_serial("HTTP: resolving ");
    log_serial(hostname);
    log_serial("\n");
    terminal_writestring("Resolving ");
    terminal_writestring(hostname);
    terminal_writestring("...\n");

    uint32_t ip = net_dns_resolve(hostname);
    if (ip == 0) {
        log_serial("HTTP: DNS resolution failed\n");
        terminal_writestring("DNS resolution failed\n");
        return -1;
    }

    /* Print resolved IP */
    log_serial("HTTP: resolved to ");
    uint8_t* ipb = (uint8_t*)&ip;
    log_hex_serial(ip);
    int_to_string(ipb[0], ip_buf); terminal_writestring(ip_buf); terminal_putchar('.');
    int_to_string(ipb[1], ip_buf); terminal_writestring(ip_buf); terminal_putchar('.');
    int_to_string(ipb[2], ip_buf); terminal_writestring(ip_buf); terminal_putchar('.');
    int_to_string(ipb[3], ip_buf); terminal_writestring(ip_buf);
    terminal_writestring("\n");

    /* ---- TCP Connect ---- */
    log_serial("HTTP: connecting...\n");
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

    log_serial("HTTP: request len=");
    log_hex_serial(req_len);
    log_serial("\n");

    /* ---- Send request ---- */
    log_serial("HTTP: sending request...\n");
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
                    log_serial("HTTP: received ");
                    log_hex_serial(n);
                    log_serial(" bytes\n");
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
            log_serial("HTTP: no response received\n");
        } else {
            log_serial("HTTP: total received ");
            log_hex_serial(total_read);
            log_serial(" bytes\n");
        }
    }

    /* ---- Close ---- */
    tcp_close();
    return 0;
}
