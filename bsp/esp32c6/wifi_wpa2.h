#ifndef ZEN_ESP32C6_WIFI_WPA2_H
#define ZEN_ESP32C6_WIFI_WPA2_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// Freestanding WPA2-PSK Cryptographic Primitives (Host & Target Testable)
// ---------------------------------------------------------------------------
void wpa2_sha1_vec(size_t num_elem, const uint8_t *addr[], const size_t *len, uint8_t mac[20]);
void wpa2_hmac_sha1_vec(const uint8_t *key, size_t key_len,
                        size_t num_elem, const uint8_t *addr[], const size_t *len,
                        uint8_t mac[20]);
void wpa2_hmac_sha1(const uint8_t *key, size_t key_len,
                    const uint8_t *data, size_t data_len,
                    uint8_t mac[20]);
void wpa2_pbkdf2_sha1(const char *passphrase, const uint8_t *ssid, size_t ssid_len,
                      uint32_t iterations, uint8_t *buf, size_t buflen);
void wpa2_prf_384(const uint8_t pmk[32], const char *label,
                  const uint8_t *data, size_t data_len,
                  uint8_t out[48]);
int  wpa2_aes_unwrap(const uint8_t kek[16], size_t n_blocks,
                     const uint8_t *cipher, uint8_t *plain);

// ---------------------------------------------------------------------------
// ESP32-C6 WPA2-PSK STA Supplicant Callbacks (wired into wifi_osi.c)
// ---------------------------------------------------------------------------
#ifndef RTOS_HOST_TEST
typedef struct {
    int            proto;
    int            pairwise_cipher;
    int            group_cipher;
    int            key_mgmt;
    int            capabilities;
    size_t         num_pmkid;
    const uint8_t *pmkid;
    int            mgmt_group_cipher;
    uint8_t        rsnxe_capa;
} wifi_wpa_ie_t;

bool wifi_wpa2_sta_init(void);
bool wifi_wpa2_sta_deinit(void);
int  wifi_wpa2_parse_wpa_ie(const uint8_t *ie, size_t ie_len, wifi_wpa_ie_t *data);
int  wifi_wpa2_sta_connect(uint8_t *bssid);
void wifi_wpa2_sta_connected(uint8_t *bssid);
void wifi_wpa2_sta_disconnected(uint8_t reason_code);
int  wifi_wpa2_sta_rx_eapol(uint8_t *src_addr, uint8_t *buf, uint32_t len);
bool wifi_wpa2_sta_in_4way(void);
void wifi_wpa2_reset(void);
#endif

#ifdef __cplusplus
}
#endif

#endif // ZEN_ESP32C6_WIFI_WPA2_H
