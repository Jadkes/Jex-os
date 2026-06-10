/**
 * @file tcp.h
 * @brief TCP/IP — minimal client stack for JexOS.
 *
 * Purpose: Provides TCP client connectivity for outbound connections
 *          (HTTP GET, etc.).  Single-connection state machine with
 *          timer-based retransmission.
 *
 * Design: The API is synchronous (blocking) for the shell context.
 *          tcp_poll() is called from the main loop to process incoming
 *          segments; the ISR delivers raw TCP data to a receive ring.
 *
 * Thread-safety: Called from shell context only.  The ISR path calls
 *                tcp_receive() which is non-re-entrant by design
 *                (single-core, cli/sti guards).
 */

#ifndef TCP_H
#define TCP_H

#include <stdint.h>

/* ----------------------------------------------------------------- */
/*  TCP Header                                                       */
/* ----------------------------------------------------------------- */

typedef struct {
    uint16_t src_port;
    uint16_t dest_port;
    uint32_t seq_num;
    uint32_t ack_num;
    uint8_t  data_offset;   /* Top 4 bits: header len / 4 */
    uint8_t  flags;
    uint16_t window;
    uint16_t checksum;
    uint16_t urgent_ptr;
} __attribute__((packed)) tcp_header_t;

/* TCP Flags */
#define TCP_FIN  0x01
#define TCP_SYN  0x02
#define TCP_RST  0x04
#define TCP_PSH  0x08
#define TCP_ACK  0x10
#define TCP_URG  0x20

/* TCP Options (MSS) */
#define TCP_OPT_MSS_KIND    2
#define TCP_OPT_MSS_LEN     4
#define TCP_OPT_MSS_VAL    1460

/* ----------------------------------------------------------------- */
/*  TCP States                                                       */
/* ----------------------------------------------------------------- */

enum tcp_state {
    TCP_CLOSED      = 0,
    TCP_SYN_SENT    = 1,
    TCP_ESTABLISHED = 2,
    TCP_FIN_WAIT_1  = 3,
    TCP_FIN_WAIT_2  = 4,
    TCP_TIME_WAIT   = 5,
    TCP_CLOSE_WAIT  = 6,
    TCP_LAST_ACK    = 7,
};

/* TCP pseudo-header for checksum computation */
typedef struct {
    uint32_t src_ip;
    uint32_t dest_ip;
    uint8_t  zero;
    uint8_t  protocol;
    uint16_t tcp_len;
} __attribute__((packed)) tcp_pseudo_t;

/* ----------------------------------------------------------------- */
/*  Connection State                                                 */
/* ----------------------------------------------------------------- */

#define TCP_RX_BUF_SIZE 4096
#define TCP_TX_BUF_SIZE 2048
#define TCP_MAX_RETRIES 5
#define TCP_RETRY_TIMEOUT_MS 3000    /* 3 seconds per retry */

/* ----------------------------------------------------------------- */
/*  Public API                                                       */
/* ----------------------------------------------------------------- */

/*
 * tcp_connect - Open a TCP connection to a remote host.
 *
 * Sends SYN and busy-waits for SYN+ACK.  Blocks until established
 * or the retry budget is exhausted.
 *
 * @param dest_ip   Remote IP (network byte order).
 * @param dest_port Remote port (host byte order).
 * @param src_port  Local port (host byte order, 0 = auto pick).
 * @return          0 on success (state = ESTABLISHED), -1 on failure.
 */
int tcp_connect(uint32_t dest_ip, uint16_t dest_port, uint16_t src_port);

/*
 * tcp_send - Send data on an established connection.
 *
 * Blocks until the data has been acknowledged or the retry budget
 * is exhausted.  Data may be split across multiple segments if it
 * exceeds the MSS.
 *
 * @param data  Payload buffer.
 * @param len   Length in bytes.
 * @return      Number of bytes acknowledged, or -1 on error.
 */
int tcp_send(const uint8_t* data, uint32_t len);

/*
 * tcp_receive - Copy received data into a user buffer.
 *
 * Copies up to buf_len bytes from the internal RX ring.
 *
 * @param buf      Destination buffer.
 * @param buf_len  Max bytes to read.
 * @return         Number of bytes copied, or 0 if none available.
 */
int tcp_receive(uint8_t* buf, uint32_t buf_len);

/*
 * tcp_available - Return how many bytes are in the RX buffer.
 */
int tcp_available(void);

/*
 * tcp_close - Gracefully close an established connection.
 *
 * Sends FIN and waits for FIN+ACK.  Blocks until closed or
 * the retry budget is exhausted.
 */
void tcp_close(void);

/*
 * tcp_abort - Forcefully reset the connection (send RST).
 */
void tcp_abort(void);

/*
 * tcp_get_state - Return the current connection state.
 */
int tcp_get_state(void);

/*
 * tcp_get_remote_ip - Return the remote IP of the current connection.
 */
uint32_t tcp_get_remote_ip(void);

/*
 * tcp_get_remote_port - Return the remote port (host byte order).
 */
uint16_t tcp_get_remote_port(void);

/*
 * http_get - Perform an HTTP GET request and print the response.
 *
 * Connects to host:port, sends "GET /path HTTP/1.0\r\nHost: host\r\n\r\n",
 * reads the response, and prints it to the terminal.
 *
 * This is a blocking convenience wrapper around the raw TCP API.
 *
 * @param hostname  Remote hostname (e.g. "example.com").
 * @param port      Remote port (default 80).
 * @param path      Request path (e.g. "/").
 * @return          0 on success, -1 on failure.
 */
int http_get(const char* hostname, uint16_t port, const char* path);

#endif /* TCP_H */
