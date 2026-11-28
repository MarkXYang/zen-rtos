// ZenRTOS Freestanding WPA2-PSK Crypto & 4-Way EAPOL STA Supplicant for ESP32-C6
// Implements SHA-1, HMAC-SHA1, PBKDF2-SHA1 (4096 iter), IEEE 802.11i PRF-384,
// RFC 3394 AES-128 Key Unwrap, RSN IE parser, and 4-Way + Group Key Handshake.

#include "wifi_wpa2.h"

#ifdef RTOS_HOST_TEST
#include <string.h>
#else
extern void   *memcpy(void *dst, const void *src, size_t n);
extern void   *memset(void *dst, int c, size_t n);
extern int     memcmp(const void *a, const void *b, size_t n);
extern size_t  strlen(const char *s);
extern void    bsp_uart_puts(const char *s);
extern int     snprintf(char *buf, size_t cap, const char *fmt, ...);
#endif

// ===========================================================================
// 1. SHA-1 & HMAC-SHA1 (FIPS 180-4 / RFC 2104)
// ===========================================================================
typedef struct {
    uint32_t state[5];
    uint64_t bitcount;
    uint8_t  buf[64];
} sha1_ctx_t;

static inline uint32_t rol32(uint32_t v, unsigned n) {
    return (v << n) | (v >> (32U - n));
}

static inline uint32_t be32_load(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  | (uint32_t)p[3];
}

static inline void be32_store(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)(v);
}

static void sha1_transform(uint32_t state[5], const uint8_t block[64]) {
    uint32_t w[80];
    for (int i = 0; i < 16; i++) {
        w[i] = be32_load(block + i * 4);
    }
    for (int i = 16; i < 80; i++) {
        w[i] = rol32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    }

    uint32_t a = state[0], b = state[1], c = state[2], d = state[3], e = state[4];
    for (int i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20) {
            f = (b & c) | ((~b) & d);
            k = 0x5A827999UL;
        } else if (i < 40) {
            f = b ^ c ^ d;
            k = 0x6ED9EBA1UL;
        } else if (i < 60) {
            f = (b & c) | (b & d) | (c & d);
            k = 0x8F1BBCDCUL;
        } else {
            f = b ^ c ^ d;
            k = 0xCA62C1D6UL;
        }
        uint32_t tmp = rol32(a, 5) + f + e + k + w[i];
        e = d;
        d = c;
        c = rol32(b, 30);
        b = a;
        a = tmp;
    }

    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
}

static void sha1_init(sha1_ctx_t *ctx) {
    ctx->state[0] = 0x67452301UL;
    ctx->state[1] = 0xEFCDAB89UL;
    ctx->state[2] = 0x98BADCFEUL;
    ctx->state[3] = 0x10325476UL;
    ctx->state[4] = 0xC3D2E1F0UL;
    ctx->bitcount = 0;
}

static void sha1_update(sha1_ctx_t *ctx, const uint8_t *data, size_t len) {
    size_t idx = (size_t)((ctx->bitcount >> 3) & 63ULL);
    ctx->bitcount += (uint64_t)len << 3;
    size_t i = 0;
    if (idx > 0) {
        size_t fill = 64U - idx;
        if (len < fill) {
            memcpy(ctx->buf + idx, data, len);
            return;
        }
        memcpy(ctx->buf + idx, data, fill);
        sha1_transform(ctx->state, ctx->buf);
        i += fill;
    }
    while (i + 64U <= len) {
        sha1_transform(ctx->state, data + i);
        i += 64U;
    }
    if (i < len) {
        memcpy(ctx->buf, data + i, len - i);
    }
}

static void sha1_final(sha1_ctx_t *ctx, uint8_t digest[20]) {
    uint64_t bits = ctx->bitcount;
    uint8_t pad = 0x80U;
    sha1_update(ctx, &pad, 1);
    uint8_t zero = 0U;
    while (((ctx->bitcount >> 3) & 63ULL) != 56ULL) {
        sha1_update(ctx, &zero, 1);
    }
    uint8_t len_be[8];
    be32_store(len_be, (uint32_t)(bits >> 32));
    be32_store(len_be + 4, (uint32_t)(bits & 0xFFFFFFFFULL));
    sha1_update(ctx, len_be, 8);
    for (int i = 0; i < 5; i++) {
        be32_store(digest + i * 4, ctx->state[i]);
    }
}

void wpa2_sha1_vec(size_t num_elem, const uint8_t *addr[], const size_t *len, uint8_t mac[20]) {
    sha1_ctx_t ctx;
    sha1_init(&ctx);
    for (size_t i = 0; i < num_elem; i++) {
        if (addr[i] && len[i] > 0) {
            sha1_update(&ctx, addr[i], len[i]);
        }
    }
    sha1_final(&ctx, mac);
}

void wpa2_hmac_sha1_vec(const uint8_t *key, size_t key_len,
                        size_t num_elem, const uint8_t *addr[], const size_t *len,
                        uint8_t mac[20]) {
    uint8_t k_pad[64];
    uint8_t tk[20];
    if (key_len > 64U) {
        const uint8_t *ka[1] = { key };
        const size_t   kl[1] = { key_len };
        wpa2_sha1_vec(1, ka, kl, tk);
        key = tk;
        key_len = 20U;
    }

    memset(k_pad, 0, sizeof(k_pad));
    memcpy(k_pad, key, key_len);
    for (int i = 0; i < 64; i++) k_pad[i] ^= 0x36U;

    sha1_ctx_t ctx;
    sha1_init(&ctx);
    sha1_update(&ctx, k_pad, 64U);
    for (size_t i = 0; i < num_elem; i++) {
        if (addr[i] && len[i] > 0) sha1_update(&ctx, addr[i], len[i]);
    }
    uint8_t inner[20];
    sha1_final(&ctx, inner);

    memset(k_pad, 0, sizeof(k_pad));
    memcpy(k_pad, key, key_len);
    for (int i = 0; i < 64; i++) k_pad[i] ^= 0x5CU;

    sha1_init(&ctx);
    sha1_update(&ctx, k_pad, 64U);
    sha1_update(&ctx, inner, 20U);
    sha1_final(&ctx, mac);
}

void wpa2_hmac_sha1(const uint8_t *key, size_t key_len,
                    const uint8_t *data, size_t data_len,
                    uint8_t mac[20]) {
    const uint8_t *a[1] = { data };
    const size_t   l[1] = { data_len };
    wpa2_hmac_sha1_vec(key, key_len, 1, a, l, mac);
}

// ===========================================================================
// 2. PBKDF2-HMAC-SHA1 (RFC 2898 / IEEE 802.11i) & PRF-384
// ===========================================================================
static void pbkdf2_sha1_f(const uint8_t *pass, size_t pass_len,
                          const uint8_t *ssid, size_t ssid_len,
                          uint32_t iterations, uint32_t count, uint8_t digest[20]) {
    uint8_t cnt_be[4];
    be32_store(cnt_be, count);
    const uint8_t *a[2] = { ssid, cnt_be };
    const size_t   l[2] = { ssid_len, 4U };

    uint8_t tmp[20];
    wpa2_hmac_sha1_vec(pass, pass_len, 2, a, l, tmp);
    memcpy(digest, tmp, 20U);

    for (uint32_t i = 1; i < iterations; i++) {
        uint8_t tmp2[20];
        wpa2_hmac_sha1(pass, pass_len, tmp, 20U, tmp2);
        memcpy(tmp, tmp2, 20U);
        for (int j = 0; j < 20; j++) {
            digest[j] ^= tmp[j];
        }
    }
}

void wpa2_pbkdf2_sha1(const char *passphrase, const uint8_t *ssid, size_t ssid_len,
                      uint32_t iterations, uint8_t *buf, size_t buflen) {
    size_t pass_len = strlen(passphrase);
    uint32_t count = 1;
    uint8_t *pos = buf;
    size_t left = buflen;
    while (left > 0) {
        uint8_t digest[20];
        pbkdf2_sha1_f((const uint8_t *)passphrase, pass_len, ssid, ssid_len, iterations, count, digest);
        size_t take = (left < 20U) ? left : 20U;
        memcpy(pos, digest, take);
        pos  += take;
        left -= take;
        count++;
    }
}

void wpa2_prf_384(const uint8_t pmk[32], const char *label,
                  const uint8_t *data, size_t data_len,
                  uint8_t out[48]) {
    uint8_t zero = 0U;
    uint8_t counter = 0U;
    size_t label_len = strlen(label);
    const uint8_t *a[4] = { (const uint8_t *)label, &zero, data, &counter };
    const size_t   l[4] = { label_len, 1U, data_len, 1U };

    size_t pos = 0;
    while (pos < 48U) {
        uint8_t hash[20];
        wpa2_hmac_sha1_vec(pmk, 32U, 4, a, l, hash);
        size_t take = (48U - pos < 20U) ? (48U - pos) : 20U;
        memcpy(out + pos, hash, take);
        pos += take;
        counter++;
    }
}

// ===========================================================================
// 3. Compact AES-128 Decryption & RFC 3394 Key Unwrap (for GTK in EAPOL Msg 3)
// ===========================================================================
static const uint8_t s_aes_sbox[256] = {
    0x63,0x7c,0x77,0x7b,0xf2,0x6b,0x6f,0xc5,0x30,0x01,0x67,0x2b,0xfe,0xd7,0xab,0x76,
    0xca,0x82,0xc9,0x7d,0xfa,0x59,0x47,0xf0,0xad,0xd4,0xa2,0xaf,0x9c,0xa4,0x72,0xc0,
    0xb7,0xfd,0x93,0x26,0x36,0x3f,0xf7,0xcc,0x34,0xa5,0xe5,0xf1,0x71,0xd8,0x31,0x15,
    0x04,0xc7,0x23,0xc3,0x18,0x96,0x05,0x9a,0x07,0x12,0x80,0xe2,0xeb,0x27,0xb2,0x75,
    0x09,0x83,0x2c,0x1a,0x1b,0x6e,0x5a,0xa0,0x52,0x3b,0xd6,0xb3,0x29,0xe3,0x2f,0x84,
    0x53,0xd1,0x00,0xed,0x20,0xfc,0xb1,0x5b,0x6a,0xcb,0xbe,0x39,0x4a,0x4c,0x58,0xcf,
    0xd0,0xef,0xaa,0xfb,0x43,0x4d,0x33,0x85,0x45,0xf9,0x02,0x7f,0x50,0x3c,0x9f,0xa8,
    0x51,0xa3,0x40,0x8f,0x92,0x9d,0x38,0xf5,0xbc,0xb6,0xda,0x21,0x10,0xff,0xf3,0xd2,
    0xcd,0x0c,0x13,0xec,0x5f,0x97,0x44,0x17,0xc4,0xa7,0x7e,0x3d,0x64,0x5d,0x19,0x73,
    0x60,0x81,0x4f,0xdc,0x22,0x2a,0x90,0x88,0x46,0xee,0xb8,0x14,0xde,0x5e,0x0b,0xdb,
    0xe0,0x32,0x3a,0x0a,0x49,0x06,0x24,0x5c,0xc2,0xd3,0xac,0x62,0x91,0x95,0xe4,0x79,
    0xe7,0xc8,0x37,0x6d,0x8d,0xd5,0x4e,0xa9,0x6c,0x56,0xf4,0xea,0x65,0x7a,0xae,0x08,
    0xba,0x78,0x25,0x2e,0x1c,0xa6,0xb4,0xc6,0xe8,0xdd,0x74,0x1f,0x4b,0xbd,0x8b,0x8a,
    0x70,0x3e,0xb5,0x66,0x48,0x03,0xf6,0x0e,0x61,0x35,0x57,0xb9,0x86,0xc1,0x1d,0x9e,
    0xe1,0xf8,0x98,0x11,0x69,0xd9,0x8e,0x94,0x9b,0x1e,0x87,0xe9,0xce,0x55,0x28,0xdf,
    0x8c,0xa1,0x89,0x0d,0xbf,0xe6,0x42,0x68,0x41,0x99,0x2d,0x0f,0xb0,0x54,0xbb,0x16
};

static const uint8_t s_aes_rsbox[256] = {
    0x52,0x09,0x6a,0xd5,0x30,0x36,0xa5,0x38,0xbf,0x40,0xa3,0x9e,0x81,0xf3,0xd7,0xfb,
    0x7c,0xe3,0x39,0x82,0x9b,0x2f,0xff,0x87,0x34,0x8e,0x43,0x44,0xc4,0xde,0xe9,0xcb,
    0x54,0x7b,0x94,0x32,0xa6,0xc2,0x23,0x3d,0xee,0x4c,0x95,0x0b,0x42,0xfa,0xc3,0x4e,
    0x08,0x2e,0xa1,0x66,0x28,0xd9,0x24,0xb2,0x76,0x5b,0xa2,0x49,0x6d,0x8b,0xd1,0x25,
    0x72,0xf8,0xf6,0x64,0x86,0x68,0x98,0x16,0xd4,0xa4,0x5c,0xcc,0x5d,0x65,0xb6,0x92,
    0x6c,0x70,0x48,0x50,0xfd,0xed,0xb9,0xda,0x5e,0x15,0x46,0x57,0xa7,0x8d,0x9d,0x84,
    0x90,0xd8,0xab,0x00,0x8c,0xbc,0xd3,0x0a,0xf7,0xe4,0x58,0x05,0xb8,0xb3,0x45,0x06,
    0xd0,0x2c,0x1e,0x8f,0xca,0x3f,0x0f,0x02,0xc1,0xaf,0xbd,0x03,0x01,0x13,0x8a,0x6b,
    0x3a,0x91,0x11,0x41,0x4f,0x67,0xdc,0xea,0x97,0xf2,0xcf,0xce,0xf0,0xb4,0xe6,0x73,
    0x96,0xac,0x74,0x22,0xe7,0xad,0x35,0x85,0xe2,0xf9,0x37,0xe8,0x1c,0x75,0xdf,0x6e,
    0x47,0xf1,0x1a,0x71,0x1d,0x29,0xc5,0x89,0x6f,0xb7,0x62,0x0e,0xaa,0x18,0xbe,0x1b,
    0xfc,0x56,0x3e,0x4b,0xc6,0xd2,0x79,0x20,0x9a,0xdb,0xc0,0xfe,0x78,0xcd,0x5a,0xf4,
    0x1f,0xdd,0xa8,0x33,0x88,0x07,0xc7,0x31,0xb1,0x12,0x10,0x59,0x27,0x80,0xec,0x5f,
    0x60,0x51,0x7f,0xa9,0x19,0xb5,0x4a,0x0d,0x2d,0xe5,0x7a,0x9f,0x93,0xc9,0x9c,0xef,
    0xa0,0xe0,0x3b,0x4d,0xae,0x2a,0xf5,0xb0,0xc8,0xeb,0xbb,0x3c,0x83,0x53,0x99,0x61,
    0x17,0x2b,0x04,0x7e,0xba,0x77,0xd6,0x26,0xe1,0x69,0x14,0x63,0x55,0x21,0x0c,0x7d
};

static inline uint8_t gf_xtime(uint8_t x) {
    return (uint8_t)((x << 1) ^ ((x & 0x80U) ? 0x1BU : 0x00U));
}

static uint8_t gf_mul(uint8_t a, uint8_t b) {
    uint8_t res = 0;
    while (b) {
        if (b & 1U) res ^= a;
        a = gf_xtime(a);
        b >>= 1;
    }
    return res;
}

static void aes128_key_expand(const uint8_t key[16], uint8_t rk[176]) {
    static const uint8_t rcon[10] = { 0x01,0x02,0x04,0x08,0x10,0x20,0x40,0x80,0x1b,0x36 };
    memcpy(rk, key, 16U);
    for (int i = 4; i < 44; i++) {
        uint8_t t[4];
        memcpy(t, rk + (i - 1) * 4, 4U);
        if ((i & 3) == 0) {
            uint8_t tmp = t[0];
            t[0] = s_aes_sbox[t[1]] ^ rcon[(i / 4) - 1];
            t[1] = s_aes_sbox[t[2]];
            t[2] = s_aes_sbox[t[3]];
            t[3] = s_aes_sbox[tmp];
        }
        for (int j = 0; j < 4; j++) {
            rk[i * 4 + j] = rk[(i - 4) * 4 + j] ^ t[j];
        }
    }
}

static void aes128_decrypt_block(const uint8_t rk[176], const uint8_t in[16], uint8_t out[16]) {
    uint8_t s[16];
    for (int i = 0; i < 16; i++) s[i] = in[i] ^ rk[160 + i];

    for (int round = 9; round >= 0; round--) {
        // InvShiftRows
        uint8_t t;
        t = s[13]; s[13] = s[9];  s[9]  = s[5];  s[5]  = s[1];  s[1]  = t;
        t = s[2];  s[2]  = s[10]; s[10] = t;
        t = s[6];  s[6]  = s[14]; s[14] = t;
        t = s[3];  s[3]  = s[7];  s[7]  = s[11]; s[11] = s[15]; s[15] = t;

        // InvSubBytes + AddRoundKey
        for (int i = 0; i < 16; i++) {
            s[i] = s_aes_rsbox[s[i]] ^ rk[round * 16 + i];
        }

        // InvMixColumns (skip on final round 0)
        if (round > 0) {
            for (int c = 0; c < 4; c++) {
                uint8_t a0 = s[c*4+0], a1 = s[c*4+1], a2 = s[c*4+2], a3 = s[c*4+3];
                s[c*4+0] = gf_mul(a0, 14) ^ gf_mul(a1, 11) ^ gf_mul(a2, 13) ^ gf_mul(a3, 9);
                s[c*4+1] = gf_mul(a0, 9)  ^ gf_mul(a1, 14) ^ gf_mul(a2, 11) ^ gf_mul(a3, 13);
                s[c*4+2] = gf_mul(a0, 13) ^ gf_mul(a1, 9)  ^ gf_mul(a2, 14) ^ gf_mul(a3, 11);
                s[c*4+3] = gf_mul(a0, 11) ^ gf_mul(a1, 13) ^ gf_mul(a2, 9)  ^ gf_mul(a3, 14);
            }
        }
    }
    memcpy(out, s, 16U);
}

int wpa2_aes_unwrap(const uint8_t kek[16], size_t n_blocks,
                    const uint8_t *cipher, uint8_t *plain) {
    if (n_blocks == 0 || !cipher || !plain) return -1;
    uint8_t rk[176];
    aes128_key_expand(kek, rk);

    uint8_t a[8];
    memcpy(a, cipher, 8U);
    memcpy(plain, cipher + 8U, n_blocks * 8U);

    for (int j = 5; j >= 0; j--) {
        for (int i = (int)n_blocks; i >= 1; i--) {
            uint64_t t = (uint64_t)n_blocks * (uint64_t)j + (uint64_t)i;
            uint8_t b[16];
            memcpy(b, a, 8U);
            b[7] ^= (uint8_t)(t & 0xFFU);
            b[6] ^= (uint8_t)((t >> 8) & 0xFFU);
            b[5] ^= (uint8_t)((t >> 16) & 0xFFU);
            b[4] ^= (uint8_t)((t >> 24) & 0xFFU);
            memcpy(b + 8U, plain + (size_t)(i - 1) * 8U, 8U);
            aes128_decrypt_block(rk, b, b);
            memcpy(a, b, 8U);
            memcpy(plain + (size_t)(i - 1) * 8U, b + 8U, 8U);
        }
    }

    for (int i = 0; i < 8; i++) {
        if (a[i] != 0xA6U) return -1;
    }
    return 0;
}

// ===========================================================================
// 4. ESP32-C6 WPA2-PSK STA Supplicant & 4-Way EAPOL State Machine
// ===========================================================================
#ifndef RTOS_HOST_TEST

#include "esp_wifi.h"

#define WIFI_WPA_ALG_NONE   0
#define WIFI_WPA_ALG_TKIP   2
#define WIFI_WPA_ALG_CCMP   3

#define WIFI_APPIE_WPA      3
#define WIFI_APPIE_RSN      4

#define KEY_FLAG_MODIFY     (1U << 0)
#define KEY_FLAG_RX         (1U << 2)
#define KEY_FLAG_TX         (1U << 3)
#define KEY_FLAG_GROUP      (1U << 4)
#define KEY_FLAG_PAIRWISE   (1U << 5)

struct wifi_ssid_compat {
    int     len;
    uint8_t ssid[32];
};

typedef void (*eapol_txcb_t)(uint8_t *eapol_payload, size_t len, bool tx_failure);

extern int       esp_wifi_sta_connect_internal(const uint8_t *bssid);
extern int       esp_wifi_set_appie_internal(uint8_t type, uint8_t *ie, uint16_t len, uint8_t flag);
extern int       esp_wifi_register_eapol_txdonecb_internal(eapol_txcb_t cb);
extern int       esp_wifi_set_sta_key_internal(int alg, uint8_t *addr, int key_idx, int set_tx,
                                               uint8_t *seq, size_t seq_len, uint8_t *key, size_t key_len,
                                               unsigned int key_flag);
extern bool      esp_wifi_auth_done_internal(void);
extern int       esp_wifi_get_macaddr_internal(uint8_t if_index, uint8_t *macaddr);
extern struct wifi_ssid_compat *esp_wifi_sta_get_prof_ssid_internal(void);
extern uint8_t  *esp_wifi_sta_get_prof_password_internal(void);
extern uint8_t   esp_wifi_sta_get_pairwise_cipher_internal(void);
extern uint8_t   esp_wifi_sta_get_group_cipher_internal(void);
extern bool      esp_wifi_sta_prof_is_wpa_internal(void);
extern bool      esp_wifi_sta_prof_is_rsn_internal(void);
extern esp_err_t esp_wifi_internal_tx(wifi_interface_t ifx, void *buffer, uint16_t len);
extern void      wifi_drv_read_mac(uint8_t mac_out[6]);

#define LP_WDEV_RNG_DATA_REG (*(volatile uint32_t *)0x600B2800UL)

static uint8_t s_sta_mac[6];
static uint8_t s_ap_bssid[6];
static uint8_t s_pmk[32];
static char    s_cached_ssid[33];
static char    s_cached_pass[65];
static bool    s_pmk_valid = false;
static uint8_t s_snonce[32];
static uint8_t s_ptk[48]; // KCK[16] | KEK[16] | TK[16]
static bool    s_in_4way = false;
static bool    s_ptk_installed = false;
static bool    s_waiting_m4_txdone = false;
static bool    s_pending_ptk_install = false;
static bool    s_has_pending_gtk = false;
static uint8_t s_pending_gtk[32];
static size_t  s_pending_gtk_len = 0;
static uint8_t s_pending_gtk_idx = 0;
static uint8_t s_pending_gtk_rsc[6];
static uint8_t s_assoc_ie[24];
static uint16_t s_assoc_ie_len = 0;

static void fill_random_bytes(uint8_t *buf, size_t len) {
    for (size_t i = 0; i < len; i++) {
        buf[i] = (uint8_t)(LP_WDEV_RNG_DATA_REG & 0xFFU);
    }
}

static int hex_nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool try_parse_hex_pmk(const char *s, uint8_t out[32]) {
    for (int i = 0; i < 32; i++) {
        int hi = hex_nibble(s[i * 2]);
        int lo = hex_nibble(s[i * 2 + 1]);
        if (hi < 0 || lo < 0) return false;
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return true;
}

static int cipher_oui_to_wifi_cipher(uint8_t suite_type) {
    switch (suite_type) {
        case 1:  return WIFI_CIPHER_TYPE_WEP40;
        case 2:  return WIFI_CIPHER_TYPE_TKIP;
        case 4:  return WIFI_CIPHER_TYPE_CCMP;
        case 5:  return WIFI_CIPHER_TYPE_WEP104;
        case 6:  return WIFI_CIPHER_TYPE_AES_CMAC128;
        default: return WIFI_CIPHER_TYPE_CCMP;
    }
}

int wifi_wpa2_parse_wpa_ie(const uint8_t *ie, size_t ie_len, wifi_wpa_ie_t *data) {
    if (!ie || !data || ie_len < 4U) return -1;
    memset(data, 0, sizeof(*data));

    if (ie[0] == 0x30U && ie_len >= 8U) { // IEEE 802.11i RSN IE
        uint16_t ver = (uint16_t)ie[2] | ((uint16_t)ie[3] << 8);
        if (ver != 1U) return -1;
        data->proto           = 2; // WPA_PROTO_RSN
        data->group_cipher    = cipher_oui_to_wifi_cipher(ie[7]);
        data->pairwise_cipher = WIFI_CIPHER_TYPE_CCMP;
        data->key_mgmt        = 2; // default WPA_KEY_MGMT_PSK

        size_t pos = 8U;
        if (pos + 2U <= ie_len) {
            uint16_t pc_cnt = (uint16_t)ie[pos] | ((uint16_t)ie[pos + 1U] << 8);
            pos += 2U;
            bool has_ccmp = false, has_tkip = false;
            for (uint16_t i = 0; i < pc_cnt && pos + 4U <= ie_len; i++, pos += 4U) {
                if (ie[pos] == 0x00U && ie[pos + 1U] == 0x0FU && ie[pos + 2U] == 0xACU) {
                    if (ie[pos + 3U] == 4U) has_ccmp = true;
                    else if (ie[pos + 3U] == 2U) has_tkip = true;
                }
            }
            if (has_ccmp && has_tkip) data->pairwise_cipher = WIFI_CIPHER_TYPE_TKIP_CCMP;
            else if (has_ccmp)        data->pairwise_cipher = WIFI_CIPHER_TYPE_CCMP;
            else if (has_tkip)        data->pairwise_cipher = WIFI_CIPHER_TYPE_TKIP;
        }
        if (pos + 2U <= ie_len) {
            uint16_t akm_cnt = (uint16_t)ie[pos] | ((uint16_t)ie[pos + 1U] << 8);
            pos += 2U;
            int akm_mask = 0;
            for (uint16_t i = 0; i < akm_cnt && pos + 4U <= ie_len; i++, pos += 4U) {
                if (ie[pos] == 0x00U && ie[pos + 1U] == 0x0FU && ie[pos + 2U] == 0xACU) {
                    switch (ie[pos + 3U]) {
                        case 1:  akm_mask |= (1 << 0);  break; // IEEE8021X
                        case 2:  akm_mask |= (1 << 1);  break; // PSK
                        case 6:  akm_mask |= (1 << 8);  break; // PSK_SHA256
                        case 8:  akm_mask |= (1 << 10); break; // SAE
                        case 18: akm_mask |= (1 << 22); break; // OWE
                        default: break;
                    }
                }
            }
            if (akm_mask != 0) data->key_mgmt = akm_mask;
        }
        if (pos + 2U <= ie_len) {
            data->capabilities = (int)((uint16_t)ie[pos] | ((uint16_t)ie[pos + 1U] << 8));
            pos += 2U;
        }
        if (pos + 2U <= ie_len) {
            uint16_t pmkid_cnt = (uint16_t)ie[pos] | ((uint16_t)ie[pos + 1U] << 8);
            pos += 2U;
            if (pmkid_cnt > 0 && pos + (size_t)pmkid_cnt * 16U <= ie_len) {
                data->num_pmkid = pmkid_cnt;
                data->pmkid = ie + pos;
                pos += (size_t)pmkid_cnt * 16U;
            }
        }
        if (pos + 4U <= ie_len && ie[pos] == 0x00U && ie[pos + 1U] == 0x0FU && ie[pos + 2U] == 0xACU) {
            data->mgmt_group_cipher = cipher_oui_to_wifi_cipher(ie[pos + 3U]);
        }
        return 0;
    }

    if (ie[0] == 0xDDU && ie_len >= 12U &&
        ie[2] == 0x00U && ie[3] == 0x50U && ie[4] == 0xF2U && ie[5] == 0x01U) { // Microsoft WPA1 IE
        data->proto           = 1; // WPA_PROTO_WPA
        data->group_cipher    = cipher_oui_to_wifi_cipher(ie[11]);
        data->pairwise_cipher = WIFI_CIPHER_TYPE_TKIP;
        data->key_mgmt        = 2; // WPA_KEY_MGMT_PSK
        if (ie_len >= 18U) {
            uint16_t pc_cnt = (uint16_t)ie[12] | ((uint16_t)ie[13] << 8);
            size_t pos = 14U;
            bool has_ccmp = false, has_tkip = false;
            for (uint16_t i = 0; i < pc_cnt && pos + 4U <= ie_len; i++, pos += 4U) {
                if (ie[pos + 3U] == 4U) has_ccmp = true;
                else if (ie[pos + 3U] == 2U) has_tkip = true;
            }
            if (has_ccmp && has_tkip) data->pairwise_cipher = WIFI_CIPHER_TYPE_TKIP_CCMP;
            else if (has_ccmp)        data->pairwise_cipher = WIFI_CIPHER_TYPE_CCMP;
            else if (has_tkip)        data->pairwise_cipher = WIFI_CIPHER_TYPE_TKIP;
        }
        return 0;
    }

    return -1;
}

void wifi_wpa2_reset(void) {
    s_in_4way = false;
    s_ptk_installed = false;
    s_waiting_m4_txdone = false;
    s_pending_ptk_install = false;
    s_has_pending_gtk = false;
}

static void install_gtk_now(const uint8_t *gtk, size_t gtk_len, uint8_t key_idx, const uint8_t rsc[6]) {
    int alg = (gtk_len == 32U) ? WIFI_WPA_ALG_TKIP : WIFI_WPA_ALG_CCMP;
    uint8_t seq[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    memcpy(seq, rsc, 6U);

    uint8_t key_buf[32];
    if (gtk_len > sizeof(key_buf)) return;
    memcpy(key_buf, gtk, gtk_len);
    if (alg == WIFI_WPA_ALG_TKIP && gtk_len == 32U) {
        // IEEE 802.11i GTK KDE stores [16..23]=Auth TX (STA RX), [24..31]=Auth RX (STA TX);
        // net80211 expects [16..23]=TX Michael key, [24..31]=RX Michael key.
        uint8_t tmp[8];
        memcpy(tmp, key_buf + 16U, 8U);
        memcpy(key_buf + 16U, key_buf + 24U, 8U);
        memcpy(key_buf + 24U, tmp, 8U);
    }

    (void)esp_wifi_set_sta_key_internal(alg, s_ap_bssid, (int)key_idx, 0,
                                        seq, 6U, key_buf, gtk_len,
                                        KEY_FLAG_GROUP | KEY_FLAG_RX);
}

static void wifi_wpa2_eapol_txcb(uint8_t *eapol_payload, size_t len, bool tx_failure) {
    if (!s_waiting_m4_txdone || !eapol_payload || len < 99U) return;
    if (eapol_payload[1] != 0x03U) return; // Only EAPOL-Key

    const uint8_t *key = eapol_payload + 4;
    uint16_t key_info = ((uint16_t)key[1] << 8) | key[2];
    uint16_t kd_len   = ((uint16_t)key[93] << 8) | key[94];

    // Confirm this TX-done notification is for 4-Way Handshake Message 4/4 (Secure=1, KeyDataLen=0)
    if (kd_len != 0U && (key_info & (1U << 9)) == 0U) return;

    if (tx_failure) {
        bsp_uart_puts("[wpa2] EAPOL 4/4 TX failed, awaiting AP retry\n");
        return;
    }

    s_waiting_m4_txdone = false;

    // Install Pairwise Key (PTK TK = s_ptk[32..47], 16B AES-CCMP) AFTER Msg 4/4 was sent unencrypted
    if (s_pending_ptk_install && !s_ptk_installed) {
        uint8_t zero_seq[8] = {0, 0, 0, 0, 0, 0, 0, 0};
        (void)esp_wifi_set_sta_key_internal(WIFI_WPA_ALG_CCMP, s_ap_bssid, 0, 1,
                                            zero_seq, 6U, s_ptk + 32U, 16U,
                                            KEY_FLAG_PAIRWISE | KEY_FLAG_RX | KEY_FLAG_TX);
        s_ptk_installed = true;
    }

    // Install Group Key (GTK) unwrapped from Msg 3/4
    if (s_has_pending_gtk) {
        install_gtk_now(s_pending_gtk, s_pending_gtk_len, s_pending_gtk_idx, s_pending_gtk_rsc);
        s_has_pending_gtk = false;
    }

    s_in_4way = false;
    bsp_uart_puts("[wpa2] 4-way handshake complete (PTK+GTK installed)\n");
    (void)esp_wifi_auth_done_internal();
}

bool wifi_wpa2_sta_init(void) {
    wifi_wpa2_reset();
    return esp_wifi_register_eapol_txdonecb_internal(wifi_wpa2_eapol_txcb) == 0;
}

bool wifi_wpa2_sta_deinit(void) {
    (void)esp_wifi_register_eapol_txdonecb_internal(NULL);
    wifi_wpa2_reset();
    return true;
}

// Note: esp_wifi_sta_get_{pairwise,group}_cipher_internal() return libnet80211 internal rsn_cipher enum
// (1 = TKIP, 3 = CCMP, 5 = WEP40, 6 = WEP104), NOT public wifi_cipher_type_t (where TKIP=3, CCMP=4)!
#define NET80211_CIPHER_TKIP 1U
#define NET80211_CIPHER_CCMP 3U

int wifi_wpa2_sta_connect(uint8_t *bssid) {
    if (!bssid) return -1;
    memcpy(s_ap_bssid, bssid, 6U);
    wifi_drv_read_mac(s_sta_mac);
    wifi_wpa2_reset();

    struct wifi_ssid_compat *ssid_p = esp_wifi_sta_get_prof_ssid_internal();
    uint8_t *pass_p = esp_wifi_sta_get_prof_password_internal();
    const char *pass = pass_p ? (const char *)pass_p : "";

    if (pass[0] != '\0' && ssid_p && ssid_p->len > 0) {
        size_t slen = (ssid_p->len > 32) ? 32U : (size_t)ssid_p->len;
        size_t plen = strlen(pass);
        if (plen > 64U) plen = 64U;

        if (!s_pmk_valid ||
            strlen(s_cached_ssid) != slen || memcmp(s_cached_ssid, ssid_p->ssid, slen) != 0 ||
            strlen(s_cached_pass) != plen || memcmp(s_cached_pass, pass, plen) != 0) {
            if (plen == 64U && try_parse_hex_pmk(pass, s_pmk)) {
                // Raw 256-bit hex PSK
            } else {
                wpa2_pbkdf2_sha1(pass, ssid_p->ssid, slen, 4096U, s_pmk, 32U);
            }
            memcpy(s_cached_ssid, ssid_p->ssid, slen);
            s_cached_ssid[slen] = '\0';
            memcpy(s_cached_pass, pass, plen);
            s_cached_pass[plen] = '\0';
            s_pmk_valid = true;
        }

        uint8_t grp_c = esp_wifi_sta_get_group_cipher_internal();
        uint8_t pw_c  = esp_wifi_sta_get_pairwise_cipher_internal();
        uint8_t grp_suite = (grp_c == NET80211_CIPHER_TKIP) ? 0x02U : 0x04U;
        uint8_t pw_suite  = (pw_c  == NET80211_CIPHER_TKIP) ? 0x02U : 0x04U;

        char msg[88];
        snprintf(msg, sizeof(msg), "[wpa2] Associating with %02X:%02X:%02X:%02X:%02X:%02X (pw=%s grp=%s)...\n",
                 bssid[0], bssid[1], bssid[2], bssid[3], bssid[4], bssid[5],
                 (pw_suite == 0x02U) ? "TKIP" : "CCMP",
                 (grp_suite == 0x02U) ? "TKIP" : "CCMP");
        bsp_uart_puts(msg);

        if (esp_wifi_sta_prof_is_wpa_internal() && !esp_wifi_sta_prof_is_rsn_internal()) {
            // 24-byte Microsoft WPA1 IE
            static const uint8_t wpa_tmpl[24] = {
                0xDD, 0x16,
                0x00, 0x50, 0xF2, 0x01,
                0x01, 0x00,
                0x00, 0x50, 0xF2, 0x02, // [11] = group cipher
                0x01, 0x00,
                0x00, 0x50, 0xF2, 0x04, // [17] = pairwise cipher
                0x01, 0x00,
                0x00, 0x50, 0xF2, 0x02  // PSK AKM
            };
            memcpy(s_assoc_ie, wpa_tmpl, 24U);
            s_assoc_ie[11] = grp_suite;
            s_assoc_ie[17] = pw_suite;
            s_assoc_ie_len = 24U;
            (void)esp_wifi_set_appie_internal(WIFI_APPIE_RSN, NULL, 0, 0);
            // NOTE: flag MUST be 0 so ieee80211_set_appie allocates (2 + len) bytes instead of overwriting s_assoc_ie[0..1]!
            (void)esp_wifi_set_appie_internal(WIFI_APPIE_WPA, s_assoc_ie, s_assoc_ie_len, 0);
        } else {
            // Standard 22-byte WPA2-PSK (RSN) IE: Version 1, Group, 1x Pairwise, 1x PSK AKM, Caps=0
            static const uint8_t rsn_tmpl[22] = {
                0x30, 0x14,
                0x01, 0x00,
                0x00, 0x0F, 0xAC, 0x04, // [7]  = group cipher suite
                0x01, 0x00,
                0x00, 0x0F, 0xAC, 0x04, // [13] = pairwise cipher suite
                0x01, 0x00,
                0x00, 0x0F, 0xAC, 0x02, // PSK AKM (HMAC-SHA1)
                0x00, 0x00              // RSN capabilities
            };
            memcpy(s_assoc_ie, rsn_tmpl, 22U);
            s_assoc_ie[7]  = grp_suite;
            s_assoc_ie[13] = pw_suite;
            s_assoc_ie_len = 22U;
            (void)esp_wifi_set_appie_internal(WIFI_APPIE_WPA, NULL, 0, 0);
            // NOTE: flag MUST be 0 so ieee80211_set_appie allocates (2 + len) bytes instead of overwriting s_assoc_ie[0..1]!
            (void)esp_wifi_set_appie_internal(WIFI_APPIE_RSN, s_assoc_ie, s_assoc_ie_len, 0);
        }
    } else {
        s_assoc_ie_len = 0U;
        (void)esp_wifi_set_appie_internal(WIFI_APPIE_WPA, NULL, 0, 0);
        (void)esp_wifi_set_appie_internal(WIFI_APPIE_RSN, NULL, 0, 0);
    }

    return esp_wifi_sta_connect_internal(bssid);
}

void wifi_wpa2_sta_connected(uint8_t *bssid) {
    if (bssid) memcpy(s_ap_bssid, bssid, 6U);
    s_in_4way = false;
}

void wifi_wpa2_sta_disconnected(uint8_t reason_code) {
    (void)reason_code;
    wifi_wpa2_reset();
}

bool wifi_wpa2_sta_in_4way(void) {
    return s_in_4way;
}

static void derive_ptk(const uint8_t anonce[32], const uint8_t snonce[32], uint8_t ptk_out[48]) {
    uint8_t data[76];
    if (memcmp(s_sta_mac, s_ap_bssid, 6U) < 0) {
        memcpy(data + 0,  s_sta_mac,  6U);
        memcpy(data + 6,  s_ap_bssid, 6U);
    } else {
        memcpy(data + 0,  s_ap_bssid, 6U);
        memcpy(data + 6,  s_sta_mac,  6U);
    }
    if (memcmp(snonce, anonce, 32U) < 0) {
        memcpy(data + 12, snonce, 32U);
        memcpy(data + 44, anonce, 32U);
    } else {
        memcpy(data + 12, anonce, 32U);
        memcpy(data + 44, snonce, 32U);
    }
    wpa2_prf_384(s_pmk, "Pairwise key expansion", data, sizeof(data), ptk_out);
}

static void send_eapol_key_reply(uint8_t desc_type, const uint8_t *replay_cnt, const uint8_t *snonce,
                                 uint16_t key_info, const uint8_t *key_data, uint16_t key_data_len) {
    // Ethernet II header (14B) + EAPOL header (4B) + EAPOL-Key body (95B) + key_data
    uint8_t frame[160];
    uint16_t body_len = (uint16_t)(95U + key_data_len);
    uint16_t total_len = (uint16_t)(14U + 4U + body_len);
    if (total_len > sizeof(frame)) return;
    memset(frame, 0, total_len);

    // Ethernet II header: Dst=BSSID, Src=STA_MAC, EtherType=0x888E (EAPOL)
    memcpy(frame + 0, s_ap_bssid, 6U);
    memcpy(frame + 6, s_sta_mac,  6U);
    frame[12] = 0x88U;
    frame[13] = 0x8EU;

    uint8_t *eapol = frame + 14;
    eapol[0] = 0x01U; // 802.1X-2001 version
    eapol[1] = 0x03U; // EAPOL-Key
    eapol[2] = (uint8_t)(body_len >> 8);
    eapol[3] = (uint8_t)(body_len & 0xFFU);

    uint8_t *key = eapol + 4;
    key[0] = desc_type; // 0x02 = RSN, 0xFE = WPA1
    key[1] = (uint8_t)(key_info >> 8);
    key[2] = (uint8_t)(key_info & 0xFFU);
    // Key Length = 0 in Msg 2 & Msg 4
    memcpy(key + 5, replay_cnt, 8U);
    if (snonce) memcpy(key + 13, snonce, 32U);
    // Key Data Length at offset 93..94
    key[93] = (uint8_t)(key_data_len >> 8);
    key[94] = (uint8_t)(key_data_len & 0xFFU);
    if (key_data && key_data_len > 0) {
        memcpy(key + 95, key_data, key_data_len);
    }

    // Compute HMAC-SHA1-128 MIC over entire EAPOL frame (with MIC field zeroed at key[77..92])
    uint8_t mic[20];
    wpa2_hmac_sha1(s_ptk, 16U, eapol, (size_t)(4U + body_len), mic);
    memcpy(key + 77, mic, 16U);

    (void)esp_wifi_internal_tx(WIFI_IF_STA, frame, total_len);
}

static bool verify_eapol_mic(const uint8_t *eapol, size_t eapol_len) {
    if (eapol_len < 99U || eapol_len > 512U) return false;
    uint8_t tmp[512];
    memcpy(tmp, eapol, eapol_len);
    memset(tmp + 4 + 77, 0, 16U);
    uint8_t calc[20];
    wpa2_hmac_sha1(s_ptk, 16U, tmp, eapol_len, calc);
    return memcmp(calc, eapol + 4 + 77, 16U) == 0;
}

static bool extract_gtk_kde(const uint8_t *kde_buf, size_t kde_len,
                            uint8_t out_gtk[32], size_t *out_len, uint8_t *out_idx) {
    size_t pos = 0;
    while (pos + 2U <= kde_len) {
        uint8_t id = kde_buf[pos];
        uint8_t elen = kde_buf[pos + 1U];
        if (id == 0xDDU && elen == 0U) break; // Padding
        if (pos + 2U + elen > kde_len) break;
        // IEEE 802.11i GTK KDE: 0xDD, len >= 22, OUI 00-0F-AC, Type 01
        if (id == 0xDDU && elen >= 22U &&
            kde_buf[pos + 2U] == 0x00U && kde_buf[pos + 3U] == 0x0FU &&
            kde_buf[pos + 4U] == 0xACU && kde_buf[pos + 5U] == 0x01U) {
            size_t glen = (size_t)(elen - 6U);
            if (glen <= 32U) {
                *out_idx = kde_buf[pos + 6U] & 0x03U;
                *out_len = glen;
                memcpy(out_gtk, kde_buf + pos + 8U, glen);
                return true;
            }
        }
        pos += 2U + elen;
    }
    return false;
}

int wifi_wpa2_sta_rx_eapol(uint8_t *src_addr, uint8_t *buf, uint32_t len) {
    (void)src_addr;
    if (!buf || len < 99U || !s_pmk_valid) return -1;
    if (buf[1] != 0x03U) return 0; // Only EAPOL-Key

    uint16_t body_len = ((uint16_t)buf[2] << 8) | buf[3];
    if ((uint32_t)(4U + body_len) > len || body_len < 95U) return -1;

    const uint8_t *key = buf + 4;
    uint8_t desc_type = key[0];
    if (desc_type != 0x02U && desc_type != 0xFEU) return 0; // RSN (2) or WPA1 (254)

    uint16_t key_info = ((uint16_t)key[1] << 8) | key[2];
    uint16_t key_ver  = key_info & 0x0007U;
    bool pairwise = (key_info & (1U << 3)) != 0;
    bool install  = (key_info & (1U << 6)) != 0;
    bool ack      = (key_info & (1U << 7)) != 0;
    bool mic_flag = (key_info & (1U << 8)) != 0;
    bool secure   = (key_info & (1U << 9)) != 0;
    bool enc_data = (key_info & (1U << 12)) != 0;

    const uint8_t *replay = key + 5;
    const uint8_t *anonce = key + 13;
    const uint8_t *rsc    = key + 61;
    uint16_t kd_len = ((uint16_t)key[93] << 8) | key[94];
    const uint8_t *kd = key + 95;
    if ((uint32_t)(95U + kd_len) > body_len) return -1;

    // 1. 4-Way Handshake Message 1: Pairwise=1, Ack=1, MIC=0
    if (pairwise && ack && !mic_flag) {
        s_in_4way = true;
        s_waiting_m4_txdone = false;
        fill_random_bytes(s_snonce, sizeof(s_snonce));
        derive_ptk(anonce, s_snonce, s_ptk);
        uint16_t m2_info = (uint16_t)(key_ver | (1U << 3) | (1U << 8));
        send_eapol_key_reply(desc_type, replay, s_snonce, m2_info, s_assoc_ie, s_assoc_ie_len);
        bsp_uart_puts("[wpa2] EAPOL 1/4 -> 2/4 sent\n");
        return 0;
    }

    // 2. 4-Way Handshake Message 3: Pairwise=1, Ack=1, MIC=1
    if (pairwise && ack && mic_flag) {
        if (!verify_eapol_mic(buf, (size_t)(4U + body_len))) {
            bsp_uart_puts("[wpa2] EAPOL 3/4 MIC mismatch (check Wi-Fi password)\n");
            return -1;
        }

        s_pending_ptk_install = install;
        s_has_pending_gtk = false;

        // Unwrap Encrypted Key Data (RFC 3394 AES-128 Key Unwrap) and stage GTK for txdone install
        if (enc_data && kd_len >= 24U && (kd_len & 7U) == 0 && kd_len <= 256U) {
            uint8_t plain[256];
            size_t n_blocks = (size_t)(kd_len / 8U) - 1U;
            if (wpa2_aes_unwrap(s_ptk + 16U, n_blocks, kd, plain) == 0) {
                if (extract_gtk_kde(plain, n_blocks * 8U,
                                    s_pending_gtk, &s_pending_gtk_len, &s_pending_gtk_idx)) {
                    memcpy(s_pending_gtk_rsc, rsc, 6U);
                    s_has_pending_gtk = true;
                }
            }
        }

        // Mark waiting BEFOREesp_wifi_internal_tx so TX-done ISR/task cannot race ahead
        s_waiting_m4_txdone = true;
        uint16_t sec_bit = secure ? (1U << 9) : 0U;
        uint16_t m4_info = (uint16_t)(key_ver | (1U << 3) | (1U << 8) | sec_bit);
        send_eapol_key_reply(desc_type, replay, NULL, m4_info, NULL, 0);
        bsp_uart_puts("[wpa2] EAPOL 3/4 -> 4/4 sent\n");
        return 0;
    }

    // 3. Group Key Handshake Message 1 (GTK rekey): Pairwise=0, Ack=1, MIC=1, Secure=1
    if (!pairwise && ack && mic_flag && secure && s_ptk_installed) {
        if (!verify_eapol_mic(buf, (size_t)(4U + body_len))) return -1;
        if (enc_data && kd_len >= 24U && (kd_len & 7U) == 0 && kd_len <= 256U) {
            uint8_t plain[256];
            size_t n_blocks = (size_t)(kd_len / 8U) - 1U;
            uint8_t gtk[32];
            size_t gtk_len = 0;
            uint8_t gtk_idx = 0;
            if (wpa2_aes_unwrap(s_ptk + 16U, n_blocks, kd, plain) == 0 &&
                extract_gtk_kde(plain, n_blocks * 8U, gtk, &gtk_len, &gtk_idx)) {
                install_gtk_now(gtk, gtk_len, gtk_idx, rsc);
            }
        }
        uint16_t gm2_info = (uint16_t)(key_ver | (1U << 8) | (1U << 9));
        send_eapol_key_reply(desc_type, replay, NULL, gm2_info, NULL, 0);
        return 0;
    }

    return 0;
}

#endif // !RTOS_HOST_TEST
