// Minimal freestanding libc & 64-bit integer runtime shims for ESP32-C6 Wi-Fi blobs
// Allows linking GCC-compiled Espressif static archives under clang -nostdlib.

#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>

extern void *wifi_heap_malloc(size_t size);
extern void *wifi_heap_calloc(size_t nmemb, size_t size);
extern void *wifi_heap_realloc(void *ptr, size_t size);
extern void  wifi_heap_free(void *ptr);

void *malloc(size_t size)               { return wifi_heap_malloc(size); }
void *calloc(size_t n, size_t s)        { return wifi_heap_calloc(n, s); }
void *realloc(void *p, size_t s)        { return wifi_heap_realloc(p, s); }
void  free(void *p)                     { wifi_heap_free(p); }

void *memcpy(void *dst, const void *src, size_t n) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    while (n--) *d++ = *s++;
    return dst;
}

void *memmove(void *dst, const void *src, size_t n) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    if (d < s) {
        while (n--) *d++ = *s++;
    } else if (d > s) {
        d += n;
        s += n;
        while (n--) *--d = *--s;
    }
    return dst;
}

void *memset(void *dst, int c, size_t n) {
    uint8_t *d = (uint8_t *)dst;
    while (n--) *d++ = (uint8_t)c;
    return dst;
}

int memcmp(const void *a, const void *b, size_t n) {
    const uint8_t *p = (const uint8_t *)a;
    const uint8_t *q = (const uint8_t *)b;
    while (n--) {
        if (*p != *q) return (int)*p - (int)*q;
        p++;
        q++;
    }
    return 0;
}

size_t strlen(const char *s) {
    size_t n = 0;
    while (s && s[n]) n++;
    return n;
}

size_t strnlen(const char *s, size_t maxlen) {
    size_t n = 0;
    while (s && n < maxlen && s[n]) n++;
    return n;
}

char *strcpy(char *dst, const char *src) {
    char *r = dst;
    while ((*dst++ = *src++) != '\0') {}
    return r;
}

char *strncpy(char *dst, const char *src, size_t n) {
    size_t i = 0;
    for (; i < n && src[i]; i++) dst[i] = src[i];
    for (; i < n; i++) dst[i] = '\0';
    return dst;
}

int strcmp(const char *a, const char *b) {
    while (*a && (*a == *b)) { a++; b++; }
    return (int)(uint8_t)*a - (int)(uint8_t)*b;
}

int strncmp(const char *a, const char *b, size_t n) {
    while (n-- > 0) {
        if (*a != *b) return (int)(uint8_t)*a - (int)(uint8_t)*b;
        if (*a == '\0') return 0;
        a++; b++;
    }
    return 0;
}

char *strchr(const char *s, int c) {
    while (*s) {
        if (*s == (char)c) return (char *)s;
        s++;
    }
    return (c == 0) ? (char *)s : NULL;
}

int abs(int v) {
    return (v < 0) ? -v : v;
}

// Minimal vsnprintf / snprintf for Wi-Fi debug log formatting
static void put_ch(char *buf, size_t cap, size_t *pos, char c) {
    if (buf && cap > 0 && *pos + 1 < cap) {
        buf[*pos] = c;
    }
    (*pos)++;
}

static void put_uint_pad(char *buf, size_t cap, size_t *pos, uint32_t v, uint32_t base, int upper, int width, char pad) {
    char tmp[12];
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    int i = 0;
    if (v == 0) {
        tmp[i++] = '0';
    } else {
        while (v > 0 && i < 12) {
            tmp[i++] = digits[v % base];
            v /= base;
        }
    }
    while (i < width && i < 12) tmp[i++] = pad;
    while (i > 0) put_ch(buf, cap, pos, tmp[--i]);
}

int vsnprintf(char *buf, size_t cap, const char *fmt, va_list ap) {
    size_t pos = 0;
    while (fmt && *fmt) {
        if (*fmt != '%') {
            put_ch(buf, cap, &pos, *fmt++);
            continue;
        }
        fmt++;
        char pad = ' ';
        int width = 0;
        if (*fmt == '0') { pad = '0'; fmt++; }
        while (*fmt == '-' || *fmt == '+' || *fmt == ' ') fmt++;
        while (*fmt >= '0' && *fmt <= '9') {
            width = width * 10 + (*fmt - '0');
            fmt++;
        }
        if (*fmt == 'l') fmt++;
        if (*fmt == 'l') fmt++;
        switch (*fmt) {
            case 's': {
                const char *s = va_arg(ap, const char *);
                if (!s) s = "(null)";
                while (*s) put_ch(buf, cap, &pos, *s++);
                break;
            }
            case 'd':
            case 'i': {
                int v = va_arg(ap, int);
                if (v < 0) {
                    put_ch(buf, cap, &pos, '-');
                    put_uint_pad(buf, cap, &pos, (uint32_t)(-v), 10, 0, width > 0 ? width - 1 : 0, pad);
                } else {
                    put_uint_pad(buf, cap, &pos, (uint32_t)v, 10, 0, width, pad);
                }
                break;
            }
            case 'u':
                put_uint_pad(buf, cap, &pos, va_arg(ap, uint32_t), 10, 0, width, pad);
                break;
            case 'x':
            case 'p':
                put_uint_pad(buf, cap, &pos, va_arg(ap, uint32_t), 16, 0, width, pad);
                break;
            case 'X':
                put_uint_pad(buf, cap, &pos, va_arg(ap, uint32_t), 16, 1, width, pad);
                break;
            case 'c':
                put_ch(buf, cap, &pos, (char)va_arg(ap, int));
                break;
            case '%':
                put_ch(buf, cap, &pos, '%');
                break;
            default:
                if (*fmt) put_ch(buf, cap, &pos, *fmt);
                break;
        }
        if (*fmt) fmt++;
    }
    if (buf && cap > 0) {
        buf[(pos < cap) ? pos : (cap - 1)] = '\0';
    }
    return (int)pos;
}

int snprintf(char *buf, size_t cap, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = vsnprintf(buf, cap, fmt, ap);
    va_end(ap);
    return r;
}

int sprintf(char *buf, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = vsnprintf(buf, 256, fmt, ap);
    va_end(ap);
    return r;
}

extern void bsp_uart_putc(char c);
extern void bsp_uart_puts(const char *s);

int putchar(int c) {
    bsp_uart_putc((char)c);
    return c;
}

int puts(const char *s) {
    bsp_uart_puts(s);
    bsp_uart_putc('\n');
    return 0;
}

int net80211_printf(const char *fmt, ...) {
    char buf[128];
    va_list ap;
    va_start(ap, fmt);
    int r = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    bsp_uart_puts(buf);
    return r;
}

int pp_printf(const char *fmt, ...) {
    char buf[128];
    va_list ap;
    va_start(ap, fmt);
    int r = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    bsp_uart_puts(buf);
    return r;
}

int phy_printf(const char *fmt, ...) {
    char buf[128];
    va_list ap;
    va_start(ap, fmt);
    int r = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    bsp_uart_puts(buf);
    return r;
}

int coexist_printf(const char *fmt, ...) {
    char buf[128];
    va_list ap;
    va_start(ap, fmt);
    int r = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    bsp_uart_puts(buf);
    return r;
}

double floor(double x) {
    int64_t i = (int64_t)x;
    return (x < (double)i) ? (double)(i - 1) : (double)i;
}

static int hex_nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

int hexstr2bin(const char *hex, uint8_t *buf, size_t len) {
    for (size_t i = 0; i < len; i++) {
        int hi = hex_nibble(hex[i * 2]);
        int lo = hex_nibble(hex[i * 2 + 1]);
        if (hi < 0 || lo < 0) return -1;
        buf[i] = (uint8_t)((hi << 4) | lo);
    }
    return 0;
}

// 64-bit unsigned division / modulo helpers for RV32IMAC (-nostdlib)
uint64_t __udivdi3(uint64_t n, uint64_t d) {
    if (d == 0) return 0;
    uint64_t q = 0, r = 0;
    for (int i = 63; i >= 0; i--) {
        r = (r << 1) | ((n >> i) & 1ULL);
        if (r >= d) {
            r -= d;
            q |= (1ULL << i);
        }
    }
    return q;
}

uint64_t __umoddi3(uint64_t n, uint64_t d) {
    if (d == 0) return 0;
    uint64_t r = 0;
    for (int i = 63; i >= 0; i--) {
        r = (r << 1) | ((n >> i) & 1ULL);
        if (r >= d) {
            r -= d;
        }
    }
    return r;
}

int64_t __divdi3(int64_t a, int64_t b) {
    int neg = (a < 0) ^ (b < 0);
    uint64_t ua = (a < 0) ? (uint64_t)(-a) : (uint64_t)a;
    uint64_t ub = (b < 0) ? (uint64_t)(-b) : (uint64_t)b;
    uint64_t uq = __udivdi3(ua, ub);
    return neg ? -(int64_t)uq : (int64_t)uq;
}
