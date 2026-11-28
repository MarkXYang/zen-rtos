#ifndef ZEN_NET_H
#define ZEN_NET_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// DHCP / IPv4 interface state
#define ZEN_DHCP_OFF        0
#define ZEN_DHCP_SELECTING  1
#define ZEN_DHCP_REQUESTING 2
#define ZEN_DHCP_BOUND      3
#define ZEN_DHCP_STATIC     4

#define ZEN_ARP_TABLE_SIZE  8
#define ZEN_UDP_MAX_SOCKETS 4

typedef int32_t (*zen_net_tx_fn_t)(const uint8_t *frame, uint16_t len);

typedef struct {
    uint32_t rx_frames;
    uint32_t tx_frames;
    uint32_t rx_bytes;
    uint32_t tx_bytes;
    uint32_t arp_rx;
    uint32_t arp_tx;
    uint32_t icmp_rx;
    uint32_t icmp_tx;
    uint32_t udp_rx;
    uint32_t udp_tx;
    uint32_t checksum_drops;
} zen_net_stats_t;

// Core lifecycle & L2 driver hooks
void     zen_net_init(const uint8_t mac[6], void *tx_cb);
void     zen_net_set_link(bool up);
void     zen_net_rx_frame(const uint8_t *frame, uint16_t len);
void     zen_net_tick(uint32_t now_ms);

// IPv4 configuration & state getters (IPv4 addresses are in host byte order: 0xC0A80101 = 192.168.1.1)
void     zen_net_dhcp_start(void);
void     zen_net_set_static(uint32_t ip, uint32_t mask, uint32_t gw, uint32_t dns);
bool     zen_net_is_up(void);
bool     zen_net_link_is_up(void);
uint32_t zen_net_get_dhcp_state(void);
uint32_t zen_net_get_ip(void);
uint32_t zen_net_get_netmask(void);
uint32_t zen_net_get_gateway(void);
uint32_t zen_net_get_dns(void);
uint32_t zen_net_get_lease_rem_sec(void);
void     zen_net_get_mac(uint8_t mac_out[6]);
void     zen_net_get_stats(zen_net_stats_t *out);

// ARP cache inspection & control
void     zen_net_arp_flush(void);
void     zen_net_arp_probe(uint32_t ip);
bool     zen_net_arp_get_entry(uint32_t idx, uint32_t *out_ip, uint8_t out_mac[6], uint32_t *out_age_sec);

// Bidirectional ICMPv4 Ping
bool     zen_net_ping(uint32_t dst_ip, uint16_t seq, uint32_t timeout_ms,
                      uint32_t *out_rtt_ms, uint8_t *out_ttl);

// 4-slot UDP Socket API
int32_t  zen_udp_bind(uint16_t local_port);
void     zen_udp_unbind(int32_t slot);
uint16_t zen_udp_slot_port(int32_t slot);
int32_t  zen_udp_sendto(uint16_t src_port, uint32_t dst_ip, uint16_t dst_port,
                        const uint8_t *data, uint16_t len);
int32_t  zen_udp_recvfrom(int32_t slot, uint8_t *buf, uint16_t cap,
                          uint32_t *src_ip, uint16_t *src_port, uint32_t timeout_ms);

// RFC 1071 checksum & IPv4 string helpers
uint16_t zen_inet_checksum(const uint8_t *data, size_t len);
bool     zen_ip4_parse(const char *str, uint32_t *out_ip);
void     zen_ip4_format(uint32_t ip, char *buf, size_t cap);

#ifdef __cplusplus
}
#endif

#endif // ZEN_NET_H
