#ifndef ZEN_NET_UTILS_H
#define ZEN_NET_UTILS_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// RFC 1035 DNS A-record resolver (UDP port 53)
size_t   zen_dns_build_query(const char *host, uint16_t txid, uint8_t *buf, size_t cap);
bool     zen_dns_parse_response(const uint8_t *pkt, size_t len, uint16_t expect_txid, uint32_t *out_ip);
bool     zen_dns_resolve(const char *host, uint32_t *out_ip, uint32_t timeout_ms);

// RFC 4330 SNTPv4 UTC clock sync (UDP port 123)
bool     zen_sntp_sync(uint32_t server_ip, uint32_t timeout_ms);
bool     zen_time_is_synced(void);
uint32_t zen_time_now_utc(void);
void     zen_time_format_utc(uint32_t epoch, char *buf, size_t cap);

// Remote UDP Syslog (RFC 3164, default port 514)
void     zen_syslog_config(uint32_t server_ip, uint16_t port);
bool     zen_syslog_is_enabled(void);
uint32_t zen_syslog_get_ip(void);
uint16_t zen_syslog_get_port(void);
int32_t  zen_syslog_send(const char *msg);

#ifdef __cplusplus
}
#endif

#endif // ZEN_NET_UTILS_H
