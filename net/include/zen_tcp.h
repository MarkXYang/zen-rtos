#ifndef ZEN_TCP_H
#define ZEN_TCP_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ZEN_TCP_MAX_SOCKETS  2

#define ZEN_TCP_CLOSED       0
#define ZEN_TCP_SYN_SENT     1
#define ZEN_TCP_ESTABLISHED  2
#define ZEN_TCP_FIN_WAIT     3
#define ZEN_TCP_CLOSE_WAIT   4

// Internal hooks called by net/zen_net.zc
void     zen_tcp_init(void);
void     zen_tcp_rx_ip4(uint32_t src_ip, const uint8_t *payload, size_t len);
void     zen_tcp_tick(uint32_t now_ms);

// Public TCP Client Socket API
int32_t  zen_tcp_connect(uint32_t dst_ip, uint16_t dst_port, uint32_t timeout_ms);
int32_t  zen_tcp_send(int32_t slot, const uint8_t *data, uint16_t len, uint32_t timeout_ms);
int32_t  zen_tcp_recv(int32_t slot, uint8_t *buf, uint16_t cap, uint32_t timeout_ms);
void     zen_tcp_close(int32_t slot);
uint32_t zen_tcp_slot_state(int32_t slot);
uint16_t zen_tcp_slot_local_port(int32_t slot);
uint16_t zen_tcp_slot_remote_port(int32_t slot);

// HTTP/1.1 GET Client Helper
int32_t  zen_http_get(const char *host, uint16_t port, const char *path,
                      uint8_t *out_buf, uint16_t cap, uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif

#endif // ZEN_TCP_H
