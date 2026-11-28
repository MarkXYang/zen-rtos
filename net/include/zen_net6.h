#ifndef ZEN_NET6_H
#define ZEN_NET6_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ZEN_NDP_TABLE_SIZE 8

// Internal hooks called by net/zen_net.zc
void     zen_net6_init(const uint8_t mac[6]);
void     zen_net6_set_link(bool up);
void     zen_net6_rx_frame(const uint8_t *frame, uint16_t len);
void     zen_net6_tick(uint32_t now_ms);

// IPv6 address getters & status
bool     zen_net6_has_link_local(void);
bool     zen_net6_has_global(void);
bool     zen_net6_has_gateway(void);
void     zen_net6_get_link_local(uint8_t out[16]);
void     zen_net6_get_global(uint8_t out[16]);
void     zen_net6_get_gateway(uint8_t out[16]);

// NDP Neighbor Cache
void     zen_net6_ndp_flush(void);
bool     zen_net6_ndp_get_entry(uint32_t idx, uint8_t out_ip6[16], uint8_t out_mac[6], uint32_t *out_age_sec);

// Bidirectional ICMPv6 Ping
bool     zen_net6_ping(const uint8_t dst_ip6[16], uint16_t seq, uint32_t timeout_ms,
                       uint32_t *out_rtt_ms, uint8_t *out_hlim);

// IPv6 string helpers (RFC 5952) & ICMPv6 checksum
bool     zen_ip6_parse(const char *str, uint8_t out[16]);
void     zen_ip6_format(const uint8_t ip[16], char *buf, size_t cap);

#ifdef __cplusplus
}
#endif

#endif // ZEN_NET6_H
