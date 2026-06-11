/**
 * @file tcp.h
 * @brief TCP/IP — minimal client stack for JexOS.
 *
 * Single-connection client TCP with timer-based retransmission.
 * Shell API blocks on connect/send/close; handle_tcp runs from ISR.
 */
#ifndef TCP_H
#define TCP_H

#include <stdint.h>

/* TCP Header */

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

/* TCP States — passive-open states for HTTP server */

enum tcp_state {
    TCP_CLOSED      = 0,
    TCP_SYN_SENT    = 1,
    TCP_ESTABLISHED = 2,
    TCP_FIN_WAIT_1  = 3,
    TCP_FIN_WAIT_2  = 4,
    TCP_TIME_WAIT   = 5,
    TCP_CLOSE_WAIT  = 6,
    TCP_LAST_ACK    = 7,
    TCP_LISTEN      = 8,   /* passive open: waiting for SYN */
    TCP_SYN_RCVD    = 9,   /* SYN received, waiting for ACK */
};

/* TCP pseudo-header for checksum computation */
typedef struct {
    uint32_t src_ip;
    uint32_t dest_ip;
    uint8_t  zero;
    uint8_t  protocol;
    uint16_t tcp_len;
} __attribute__((packed)) tcp_pseudo_t;

/* Connection constants */
#define TCP_RX_BUF_SIZE 4096
#define TCP_TX_BUF_SIZE 2048
#define TCP_MAX_RETRIES 5
#define TCP_RETRY_TIMEOUT_MS 3000

/* Public API */

int tcp_connect(uint32_t dest_ip, uint16_t dest_port, uint16_t src_port);
int tcp_listen(uint16_t port);                          /* passive open on port */
int tcp_accept(void);                                   /* accept pending connection */
int tcp_send(const uint8_t* data, uint32_t len);
int tcp_receive(uint8_t* buf, uint32_t buf_len);
int tcp_available(void);
void tcp_close(void);
void tcp_abort(void);
int tcp_get_state(void);
uint32_t tcp_get_remote_ip(void);
uint16_t tcp_get_remote_port(void);
int http_get(const char* hostname, uint16_t port, const char* path);
int http_serve(uint16_t port);                          /* serve one HTTP request */

#endif /* TCP_H */
