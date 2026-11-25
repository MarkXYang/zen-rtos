// ZenRTOS ESP32-C6 Wi-Fi OSI Adapter, INTMTX/PLIC IRQ Router, Modem/PHY Shim & Driver Bridge
// Bridges Espressif's prebuilt esp32c6 static archives (libnet80211, libpp, libcore, libcoexist, libphy)
// into ZenRTOS tasks, semaphores, recursive mutexes, software timers, and 192 KB HP SRAM heap.

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdarg.h>
#include "rtos.h"

// ---------------------------------------------------------------------------
// External ZenRTOS & BSP Helpers
// ---------------------------------------------------------------------------
extern void     bsp_uart_puts(const char *s);
extern void    *wifi_heap_malloc(size_t size);
extern void    *wifi_heap_calloc(size_t nmemb, size_t size);
extern void    *wifi_heap_realloc(void *ptr, size_t size);
extern void     wifi_heap_free(void *ptr);
extern uint32_t wifi_heap_free_bytes(void);
extern int      vsnprintf(char *buf, size_t cap, const char *fmt, va_list ap);
extern void    *memcpy(void *dst, const void *src, size_t n);
extern void    *memset(void *dst, int c, size_t n);
extern size_t   strlen(const char *s);
extern char    *strncpy(char *dst, const char *src, size_t n);
extern volatile uint32_t g_isr_nest_depth;

// ---------------------------------------------------------------------------
// ESP32-C6 Hardware Registers (EFUSE, INTMTX, PLIC_MX, CLINT, MODEM, RNG)
// ---------------------------------------------------------------------------
#define EFUSE_RD_MAC_SPI_SYS_0_REG  (*(volatile uint32_t *)0x600B0844UL)
#define EFUSE_RD_MAC_SPI_SYS_1_REG  (*(volatile uint32_t *)0x600B0848UL)

#define INTMTX_BASE                 (0x60010000UL)
#define INTMTX_CORE0_MAP_REG(src)   (*(volatile uint32_t *)(INTMTX_BASE + ((src) * 4UL)))

#define PLIC_MX_BASE                (0x20001000UL)
#define PLIC_MX_ENABLE_REG          (*(volatile uint32_t *)(PLIC_MX_BASE + 0x0000UL))
#define PLIC_MX_TYPE_REG            (*(volatile uint32_t *)(PLIC_MX_BASE + 0x0004UL))
#define PLIC_MX_CLEAR_REG           (*(volatile uint32_t *)(PLIC_MX_BASE + 0x0008UL))
#define PLIC_MX_THRESH_REG          (*(volatile uint32_t *)(PLIC_MX_BASE + 0x000CUL))
#define PLIC_MX_PRI_REG(num)        (*(volatile uint32_t *)(PLIC_MX_BASE + 0x0010UL + ((num) * 4UL)))

#define CLINT_MTIME_LO_REG          (*(volatile uint32_t *)0x20001808UL)
#define CLINT_MTIME_HI_REG          (*(volatile uint32_t *)0x2000180CUL)

#define MODEM_SYSCON_CLK_CONF_REG   (*(volatile uint32_t *)0x600A9808UL)
#define MODEM_SYSCON_CLK_CONF1_REG  (*(volatile uint32_t *)0x600A980CUL)
#define MODEM_SYSCON_RST_CONF_REG   (*(volatile uint32_t *)0x600A9814UL)
#define MODEM_LPCON_CLK_CONF_REG    (*(volatile uint32_t *)0x600AF018UL)
#define MODEM_LPCON_RST_CONF_REG    (*(volatile uint32_t *)0x600AF024UL)

#define LP_WDEV_RNG_DATA_REG        (*(volatile uint32_t *)0x600B2800UL)

// ---------------------------------------------------------------------------
// ESP32-C6 External Interrupt Dispatch (called from arch/riscv/rv32/context.s)
// ---------------------------------------------------------------------------
typedef void (*ext_isr_fn_t)(void *arg);
static ext_isr_fn_t g_ext_isr_fn[32];
static void        *g_ext_isr_arg[32];

void bsp_riscv_ext_irq_dispatch(uint32_t irq_num) {
    if (irq_num < 32 && g_ext_isr_fn[irq_num] != NULL) {
        g_ext_isr_fn[irq_num](g_ext_isr_arg[irq_num]);
    }
}

// ---------------------------------------------------------------------------
// 1-Microsecond Monotonic Clock & Factory eFuse MAC Readout
// ---------------------------------------------------------------------------
int64_t bsp_timer_get_us(void) {
    uint32_t hi1, lo, hi2;
    do {
        hi1 = CLINT_MTIME_HI_REG;
        lo  = CLINT_MTIME_LO_REG;
        hi2 = CLINT_MTIME_HI_REG;
    } while (hi1 != hi2);
    uint64_t cycles = ((uint64_t)hi1 << 32) | (uint64_t)lo;
    // ESP32-C6 HP core runs at 160 MHz -> 160 cycles = 1 us
    return (int64_t)(cycles / 160ULL);
}

void wifi_drv_read_mac(uint8_t mac_out[6]) {
    uint32_t r0 = EFUSE_RD_MAC_SPI_SYS_0_REG;
    uint32_t r1 = EFUSE_RD_MAC_SPI_SYS_1_REG;
    mac_out[0] = (uint8_t)((r1 >> 8) & 0xFFU);
    mac_out[1] = (uint8_t)((r1 >> 0) & 0xFFU);
    mac_out[2] = (uint8_t)((r0 >> 24) & 0xFFU);
    mac_out[3] = (uint8_t)((r0 >> 16) & 0xFFU);
    mac_out[4] = (uint8_t)((r0 >> 8) & 0xFFU);
    mac_out[5] = (uint8_t)((r0 >> 0) & 0xFFU);
}

// ---------------------------------------------------------------------------
// Generic Byte-Payload Queue (for wifi_osi_funcs_t._queue_*)
// ---------------------------------------------------------------------------
typedef struct {
    uint8_t    *buf;
    uint32_t    item_size;
    uint32_t    capacity;
    uint32_t    head;
    uint32_t    tail;
    uint32_t    count;
    rtos_sem_t  items_avail;
    rtos_sem_t  slots_free;
} osi_queue_t;

static void *osi_queue_create(uint32_t queue_len, uint32_t item_size) {
    if (queue_len == 0 || item_size == 0) return NULL;
    osi_queue_t *q = (osi_queue_t *)wifi_heap_calloc(1, sizeof(osi_queue_t));
    if (!q) return NULL;
    q->buf = (uint8_t *)wifi_heap_malloc(queue_len * item_size);
    if (!q->buf) {
        wifi_heap_free(q);
        return NULL;
    }
    q->item_size = item_size;
    q->capacity  = queue_len;
    rtos_sem_init(&q->items_avail, 0, (int32_t)queue_len);
    rtos_sem_init(&q->slots_free, (int32_t)queue_len, (int32_t)queue_len);
    return q;
}

static void osi_queue_delete(void *queue) {
    osi_queue_t *q = (osi_queue_t *)queue;
    if (!q) return;
    if (q->buf) wifi_heap_free(q->buf);
    wifi_heap_free(q);
}

static int32_t osi_queue_send(void *queue, void *item, uint32_t block_time_tick) {
    osi_queue_t *q = (osi_queue_t *)queue;
    if (!q || !item) return 0;
    if (!rtos_sem_wait(&q->slots_free, block_time_tick)) return 0;

    uint32_t flags = rtos_irq_save();
    memcpy(q->buf + (q->head * q->item_size), item, q->item_size);
    q->head = (q->head + 1U) % q->capacity;
    q->count++;
    rtos_irq_restore(flags);

    rtos_sem_signal(&q->items_avail);
    return 1;
}

static int32_t osi_queue_send_to_front(void *queue, void *item, uint32_t block_time_tick) {
    osi_queue_t *q = (osi_queue_t *)queue;
    if (!q || !item) return 0;
    if (!rtos_sem_wait(&q->slots_free, block_time_tick)) return 0;

    uint32_t flags = rtos_irq_save();
    q->tail = (q->tail + q->capacity - 1U) % q->capacity;
    memcpy(q->buf + (q->tail * q->item_size), item, q->item_size);
    q->count++;
    rtos_irq_restore(flags);

    rtos_sem_signal(&q->items_avail);
    return 1;
}

static int32_t osi_queue_send_from_isr(void *queue, void *item, void *hptw) {
    osi_queue_t *q = (osi_queue_t *)queue;
    if (!q || !item) return 0;

    uint32_t flags = rtos_irq_save();
    if (q->count >= q->capacity) {
        rtos_irq_restore(flags);
        return 0;
    }
    if (q->slots_free.count > 0) q->slots_free.count--;
    memcpy(q->buf + (q->head * q->item_size), item, q->item_size);
    q->head = (q->head + 1U) % q->capacity;
    q->count++;
    rtos_irq_restore(flags);

    bool woken = false;
    rtos_sem_signal_from_isr(&q->items_avail, &woken);
    if (hptw && woken) *(int32_t *)hptw = 1;
    return 1;
}

static int32_t osi_queue_recv(void *queue, void *item, uint32_t block_time_tick) {
    osi_queue_t *q = (osi_queue_t *)queue;
    if (!q || !item) return 0;
    if (!rtos_sem_wait(&q->items_avail, block_time_tick)) return 0;

    uint32_t flags = rtos_irq_save();
    memcpy(item, q->buf + (q->tail * q->item_size), q->item_size);
    q->tail = (q->tail + 1U) % q->capacity;
    q->count--;
    rtos_irq_restore(flags);

    rtos_sem_signal(&q->slots_free);
    return 1;
}

static uint32_t osi_queue_msg_waiting(void *queue) {
    osi_queue_t *q = (osi_queue_t *)queue;
    return q ? q->count : 0;
}

// ---------------------------------------------------------------------------
// Event Group & Timer Wrappers
// ---------------------------------------------------------------------------
typedef struct {
    volatile uint32_t bits;
    rtos_sem_t        notify;
} osi_event_group_t;

static void *osi_event_group_create(void) {
    osi_event_group_t *eg = (osi_event_group_t *)wifi_heap_calloc(1, sizeof(osi_event_group_t));
    if (eg) rtos_sem_init(&eg->notify, 0, 1);
    return eg;
}

static void osi_event_group_delete(void *event) {
    wifi_heap_free(event);
}

static uint32_t osi_event_group_set_bits(void *event, uint32_t bits) {
    osi_event_group_t *eg = (osi_event_group_t *)event;
    if (!eg) return 0;
    uint32_t flags = rtos_irq_save();
    eg->bits |= bits;
    uint32_t v = eg->bits;
    rtos_irq_restore(flags);
    rtos_sem_signal(&eg->notify);
    return v;
}

static uint32_t osi_event_group_clear_bits(void *event, uint32_t bits) {
    osi_event_group_t *eg = (osi_event_group_t *)event;
    if (!eg) return 0;
    uint32_t flags = rtos_irq_save();
    uint32_t prev = eg->bits;
    eg->bits &= ~bits;
    rtos_irq_restore(flags);
    return prev;
}

static uint32_t osi_event_group_wait_bits(void *event, uint32_t wait_bits, int clear_on_exit,
                                          int wait_all, uint32_t block_time_tick) {
    osi_event_group_t *eg = (osi_event_group_t *)event;
    if (!eg) return 0;
    uint32_t start = rtos_get_tick();
    while (1) {
        uint32_t flags = rtos_irq_save();
        uint32_t cur = eg->bits;
        bool met = wait_all ? ((cur & wait_bits) == wait_bits) : ((cur & wait_bits) != 0);
        if (met) {
            if (clear_on_exit) eg->bits &= ~wait_bits;
            rtos_irq_restore(flags);
            return cur;
        }
        rtos_irq_restore(flags);
        if (block_time_tick == 0) return cur;
        if (block_time_tick != RTOS_WAIT_FOREVER && (rtos_get_tick() - start) >= block_time_tick) {
            return eg->bits;
        }
        rtos_sem_wait(&eg->notify, 1);
    }
}

// Espressif ets_timer struct begins with 20+ bytes; store rtos_timer_t* in its priv slot
typedef struct {
    void        *next;
    uint32_t     expire;
    uint32_t     period;
    void       (*func)(void *);
    void        *priv;
} ets_timer_compat_t;

static void osi_timer_setfn(void *ptimer, void *pfunction, void *parg) {
    ets_timer_compat_t *et = (ets_timer_compat_t *)ptimer;
    if (!et) return;
    rtos_timer_t *rt = (rtos_timer_t *)et->next;
    if (!rt) {
        rt = (rtos_timer_t *)wifi_heap_calloc(1, sizeof(rtos_timer_t));
        et->next = rt;
    }
    if (rt) {
        rtos_timer_init(rt, (rtos_timer_cb_t)pfunction, parg);
    }
}

static void osi_timer_arm(void *ptimer, uint32_t tmout_ms, bool repeat) {
    ets_timer_compat_t *et = (ets_timer_compat_t *)ptimer;
    if (!et || !et->next) return;
    rtos_timer_start((rtos_timer_t *)et->next, tmout_ms ? tmout_ms : 1U, repeat);
}

static void osi_timer_arm_us(void *ptimer, uint32_t us, bool repeat) {
    uint32_t ms = (us + 999U) / 1000U;
    osi_timer_arm(ptimer, ms ? ms : 1U, repeat);
}

static void osi_timer_disarm(void *ptimer) {
    ets_timer_compat_t *et = (ets_timer_compat_t *)ptimer;
    if (!et || !et->next) return;
    rtos_timer_stop((rtos_timer_t *)et->next);
}

static void osi_timer_done(void *ptimer) {
    ets_timer_compat_t *et = (ets_timer_compat_t *)ptimer;
    if (!et || !et->next) return;
    rtos_timer_stop((rtos_timer_t *)et->next);
    wifi_heap_free(et->next);
    et->next = NULL;
}

// ---------------------------------------------------------------------------
// Wi-Fi OSI Function Implementations
// ---------------------------------------------------------------------------
static bool osi_env_is_chip(void) { return true; }

static void osi_set_intr(int32_t cpu_no, uint32_t intr_source, uint32_t intr_num, int32_t intr_prio) {
    (void)cpu_no;
    // Remap away from CLINT-reserved slots (0, 3, 4, 7) onto CPU IRQ 9/10 if requested
    if (intr_num == 0 || intr_num == 3 || intr_num == 4 || intr_num == 7) {
        intr_num = (intr_source == 0) ? 9U : 10U;
    }
    INTMTX_CORE0_MAP_REG(intr_source) = intr_num;
    PLIC_MX_PRI_REG(intr_num) = (intr_prio > 0) ? (uint32_t)intr_prio : 1U;
    PLIC_MX_THRESH_REG = 0;
}

static void osi_clear_intr(uint32_t intr_source, uint32_t intr_num) {
    (void)intr_source;
    PLIC_MX_CLEAR_REG = (1UL << intr_num);
}

static void osi_set_isr(int32_t n, void *f, void *arg) {
    if (n == 0 || n == 3 || n == 4 || n == 7) n = 9;
    if (n >= 0 && n < 32) {
        g_ext_isr_fn[n]  = (ext_isr_fn_t)f;
        g_ext_isr_arg[n] = arg;
    }
}

static void osi_ints_on(uint32_t mask) {
    PLIC_MX_ENABLE_REG |= mask;
    __asm volatile ("csrs mie, %0" : : "r"(mask));
}

static void osi_ints_off(uint32_t mask) {
    PLIC_MX_ENABLE_REG &= ~mask;
    __asm volatile ("csrc mie, %0" : : "r"(mask));
}

static bool osi_is_from_isr(void) { return g_isr_nest_depth > 0; }

static void *osi_spin_lock_create(void) { return wifi_heap_calloc(1, sizeof(uint32_t)); }
static void  osi_spin_lock_delete(void *lock) { wifi_heap_free(lock); }
static uint32_t osi_wifi_int_disable(void *mux) { (void)mux; return rtos_irq_save(); }
static void osi_wifi_int_restore(void *mux, uint32_t tmp) { (void)mux; rtos_irq_restore(tmp); }
static void osi_task_yield_from_isr(void) { rtos_schedule(); }

static void *osi_semphr_create(uint32_t max, uint32_t init) {
    rtos_sem_t *s = (rtos_sem_t *)wifi_heap_calloc(1, sizeof(rtos_sem_t));
    if (s) rtos_sem_init(s, (int32_t)init, (int32_t)(max ? max : 1));
    return s;
}
static void osi_semphr_delete(void *s) { wifi_heap_free(s); }
static int32_t osi_semphr_take(void *s, uint32_t ticks) {
    return rtos_sem_wait((rtos_sem_t *)s, ticks) ? 1 : 0;
}
static int32_t osi_semphr_give(void *s) {
    if (g_isr_nest_depth > 0) {
        bool woken = false;
        rtos_sem_signal_from_isr((rtos_sem_t *)s, &woken);
    } else {
        rtos_sem_signal((rtos_sem_t *)s);
    }
    return 1;
}

static rtos_sem_t g_wifi_thread_sem;
static bool g_wifi_thread_sem_init = false;
static void *osi_wifi_thread_semphr_get(void) {
    if (!g_wifi_thread_sem_init) {
        rtos_sem_init(&g_wifi_thread_sem, 0, 1);
        g_wifi_thread_sem_init = true;
    }
    return &g_wifi_thread_sem;
}

static void *osi_mutex_create(void) {
    rtos_mutex_t *m = (rtos_mutex_t *)wifi_heap_calloc(1, sizeof(rtos_mutex_t));
    if (m) rtos_mutex_init(m);
    return m;
}
static void osi_mutex_delete(void *m) { wifi_heap_free(m); }
static int32_t osi_mutex_lock(void *m) { return rtos_mutex_lock((rtos_mutex_t *)m, RTOS_WAIT_FOREVER) ? 1 : 0; }
static int32_t osi_mutex_unlock(void *m) { rtos_mutex_unlock((rtos_mutex_t *)m); return 1; }

static int32_t osi_task_create_pinned(void *func, const char *name, uint32_t stack_depth,
                                      void *param, uint32_t prio, void *task_handle, uint32_t core_id) {
    (void)core_id;
    uint32_t words = (stack_depth + 3U) / 4U;
    if (words < 512U) words = 512U;
    uint32_t *stk = (uint32_t *)wifi_heap_malloc(words * 4U);
    if (!stk) return 0;
    // Invert ESP-IDF priority (higher = more urgent, 1..24) to ZenRTOS (lower = more urgent, 2..28)
    uint32_t zprio = (prio < 26U) ? (28U - prio) : 2U;
    tcb_t *t = rtos_task_create_ex((rtos_task_func_t)func, param, zprio, stk, words, name ? name : "wifi");
    if (!t) {
        wifi_heap_free(stk);
        return 0;
    }
    if (task_handle) *(void **)task_handle = t;
    return 1;
}

static int32_t osi_task_create(void *func, const char *name, uint32_t stack_depth,
                               void *param, uint32_t prio, void *task_handle) {
    return osi_task_create_pinned(func, name, stack_depth, param, prio, task_handle, 0);
}

static void osi_task_delete(void *task_handle) {
    rtos_task_delete((tcb_t *)task_handle);
}

static void osi_task_delay(uint32_t tick) { rtos_delay(tick ? tick : 1U); }
static int32_t osi_task_ms_to_tick(uint32_t ms) { return (int32_t)ms; } // 1000 Hz tick = 1 ms
static void *osi_task_get_current(void) { return rtos_get_current_task(); }
static int32_t osi_task_get_max_prio(void) { return 25; }

static void *osi_zalloc(size_t s) { return wifi_heap_calloc(1, s); }
static uint32_t osi_rand(void) { return LP_WDEV_RNG_DATA_REG; }
static unsigned long osi_random(void) { return (unsigned long)LP_WDEV_RNG_DATA_REG; }
static int osi_get_random(uint8_t *buf, size_t len) {
    for (size_t i = 0; i < len; i++) {
        buf[i] = (uint8_t)(LP_WDEV_RNG_DATA_REG & 0xFFU);
    }
    return 0;
}

static int osi_read_mac(uint8_t *mac, unsigned int type) {
    wifi_drv_read_mac(mac);
    if (type == 1U) mac[5] += 1U; // SoftAP MAC offset
    return 0;
}

static void osi_wifi_clock_enable(void) {
    MODEM_SYSCON_CLK_CONF_REG  |= 0xFFFFFFFFUL;
    MODEM_SYSCON_CLK_CONF1_REG |= 0xFFFFFFFFUL;
    MODEM_LPCON_CLK_CONF_REG   |= 0xFFFFFFFFUL;
}

static void osi_wifi_clock_disable(void) {}
static void osi_wifi_reset_mac(void) {
    MODEM_SYSCON_RST_CONF_REG |= (1UL << 8);
    MODEM_SYSCON_RST_CONF_REG &= ~(1UL << 8);
}

static void osi_noop(void) {}
static int  osi_ok_int(void) { return 0; }
static int  osi_phy_country(const char *c) { (void)c; return 0; }
static int  osi_get_time(void *t) {
    if (t) {
        int64_t us = bsp_timer_get_us();
        uint32_t *tv = (uint32_t *)t;
        tv[0] = (uint32_t)(us / 1000000LL);
        tv[1] = (uint32_t)(us % 1000000LL);
    }
    return 0;
}
static uint32_t osi_slowclk_cal_get(void) { return 28633U; }

// Wi-Fi event tracking for zensh> status
static volatile int32_t g_wifi_state = 0; // 0=UNINIT, 1=IDLE, 2=SCANNING, 3=CONNECTING, 4=CONNECTED

static int32_t osi_event_post(const char *base, int32_t id, void *data, size_t len, uint32_t wait) {
    (void)base; (void)data; (void)len; (void)wait;
    // WIFI_EVENT_STA_START=2, CONNECTED=4, DISCONNECTED=5, SCAN_DONE=1
    if (id == 2 || id == 1) g_wifi_state = 1;
    else if (id == 4)       g_wifi_state = 4;
    else if (id == 5)       g_wifi_state = 1;
    return 0;
}

// NVS stubs (Phase 1 runs with nvs_enable = 0)
static int osi_nvs_not_found_open(const char *n, unsigned int m, uint32_t *h) { (void)n; (void)m; (void)h; return 0x1102; }
static void osi_nvs_close(uint32_t h) { (void)h; }
static int osi_nvs_err_i8(uint32_t h, const char *k, int8_t v) { (void)h; (void)k; (void)v; return 0x1102; }
static int osi_nvs_get_i8(uint32_t h, const char *k, int8_t *v) { (void)h; (void)k; (void)v; return 0x1102; }
static int osi_nvs_err_u8(uint32_t h, const char *k, uint8_t v) { (void)h; (void)k; (void)v; return 0x1102; }
static int osi_nvs_get_u8(uint32_t h, const char *k, uint8_t *v) { (void)h; (void)k; (void)v; return 0x1102; }
static int osi_nvs_err_u16(uint32_t h, const char *k, uint16_t v) { (void)h; (void)k; (void)v; return 0x1102; }
static int osi_nvs_get_u16(uint32_t h, const char *k, uint16_t *v) { (void)h; (void)k; (void)v; return 0x1102; }
static int osi_nvs_commit(uint32_t h) { (void)h; return 0; }
static int osi_nvs_set_blob(uint32_t h, const char *k, const void *v, size_t l) { (void)h; (void)k; (void)v; (void)l; return 0x1102; }
static int osi_nvs_get_blob(uint32_t h, const char *k, void *v, size_t *l) { (void)h; (void)k; (void)v; (void)l; return 0x1102; }
static int osi_nvs_erase(uint32_t h, const char *k) { (void)h; (void)k; return 0; }

static void osi_log_writev(unsigned int level, const char *tag, const char *fmt, va_list args) {
    if (level > 2U) return; // Print errors & warnings only
    char buf[128];
    vsnprintf(buf, sizeof(buf), fmt, args);
    bsp_uart_puts("[wifi/");
    bsp_uart_puts(tag ? tag : "drv");
    bsp_uart_puts("] ");
    bsp_uart_puts(buf);
    bsp_uart_puts("\n");
}
static void osi_log_write(unsigned int level, const char *tag, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    osi_log_writev(level, tag, fmt, ap);
    va_end(ap);
}
static uint32_t osi_log_timestamp(void) { return rtos_get_tick(); }

// Coex & Sleep-retention stubs (Wi-Fi-only Phase 1)
static uint32_t osi_coex_status(void) { return 0; }
static void osi_coex_cond(uint32_t t, bool d) { (void)t; (void)d; }
static int  osi_coex_req(uint32_t e, uint32_t l, uint32_t d) { (void)e; (void)l; (void)d; return 0; }
static int  osi_coex_rel(uint32_t e) { (void)e; return 0; }
static int  osi_coex_ch(uint8_t p, uint8_t s) { (void)p; (void)s; return 0; }
static int  osi_coex_dur(uint32_t e, uint32_t *d) { (void)e; if (d) *d = 0; return 0; }
static int  osi_coex_pti(uint32_t e, uint8_t *p) { (void)e; if (p) *p = 0; return 0; }
static void osi_coex_schm_u32(uint32_t t, uint32_t s) { (void)t; (void)s; }
static int  osi_coex_schm_int(int v) { (void)v; return 0; }
static uint32_t osi_coex_schm_get32(void) { return 0; }
static uint8_t  osi_coex_schm_get8(void) { return 0; }
static void    *osi_coex_schm_ptr(void) { return NULL; }
static int  osi_coex_schm_cb(int a, int (*cb)(int)) { (void)a; (void)cb; return 0; }
static int  osi_coex_start_cb(int (*cb)(void)) { (void)cb; return 0; }
static void osi_regdma_noop(void *a, uint32_t b, uint32_t c) { (void)a; (void)b; (void)c; }
static void *osi_sleep_ret_noop(int id) { (void)id; return NULL; }
static int  osi_coex_flex_set(uint8_t v) { (void)v; return 0; }

// Exact ESP32-C6 wifi_osi_funcs_t layout (ESP-IDF v5.3.2, VERSION=8, MAGIC=0xDEADBEAF)
#define CONFIG_IDF_TARGET_ESP32C6 1
typedef struct wifi_osi_funcs_t {
    int32_t _version;
    bool (*_env_is_chip)(void);
    void (*_set_intr)(int32_t, uint32_t, uint32_t, int32_t);
    void (*_clear_intr)(uint32_t, uint32_t);
    void (*_set_isr)(int32_t, void *, void *);
    void (*_ints_on)(uint32_t);
    void (*_ints_off)(uint32_t);
    bool (*_is_from_isr)(void);
    void *(*_spin_lock_create)(void);
    void (*_spin_lock_delete)(void *);
    uint32_t (*_wifi_int_disable)(void *);
    void (*_wifi_int_restore)(void *, uint32_t);
    void (*_task_yield_from_isr)(void);
    void *(*_semphr_create)(uint32_t, uint32_t);
    void (*_semphr_delete)(void *);
    int32_t (*_semphr_take)(void *, uint32_t);
    int32_t (*_semphr_give)(void *);
    void *(*_wifi_thread_semphr_get)(void);
    void *(*_mutex_create)(void);
    void *(*_recursive_mutex_create)(void);
    void (*_mutex_delete)(void *);
    int32_t (*_mutex_lock)(void *);
    int32_t (*_mutex_unlock)(void *);
    void *(*_queue_create)(uint32_t, uint32_t);
    void (*_queue_delete)(void *);
    int32_t (*_queue_send)(void *, void *, uint32_t);
    int32_t (*_queue_send_from_isr)(void *, void *, void *);
    int32_t (*_queue_send_to_back)(void *, void *, uint32_t);
    int32_t (*_queue_send_to_front)(void *, void *, uint32_t);
    int32_t (*_queue_recv)(void *, void *, uint32_t);
    uint32_t (*_queue_msg_waiting)(void *);
    void *(*_event_group_create)(void);
    void (*_event_group_delete)(void *);
    uint32_t (*_event_group_set_bits)(void *, uint32_t);
    uint32_t (*_event_group_clear_bits)(void *, uint32_t);
    uint32_t (*_event_group_wait_bits)(void *, uint32_t, int, int, uint32_t);
    int32_t (*_task_create_pinned_to_core)(void *, const char *, uint32_t, void *, uint32_t, void *, uint32_t);
    int32_t (*_task_create)(void *, const char *, uint32_t, void *, uint32_t, void *);
    void (*_task_delete)(void *);
    void (*_task_delay)(uint32_t);
    int32_t (*_task_ms_to_tick)(uint32_t);
    void *(*_task_get_current_task)(void);
    int32_t (*_task_get_max_priority)(void);
    void *(*_malloc)(size_t);
    void (*_free)(void *);
    int32_t (*_event_post)(const char *, int32_t, void *, size_t, uint32_t);
    uint32_t (*_get_free_heap_size)(void);
    uint32_t (*_rand)(void);
    void (*_dport_access_stall_other_cpu_start_wrap)(void);
    void (*_dport_access_stall_other_cpu_end_wrap)(void);
    void (*_wifi_apb80m_request)(void);
    void (*_wifi_apb80m_release)(void);
    void (*_phy_disable)(void);
    void (*_phy_enable)(void);
    int (*_phy_update_country_info)(const char *);
    int (*_read_mac)(uint8_t *, unsigned int);
    void (*_timer_arm)(void *, uint32_t, bool);
    void (*_timer_disarm)(void *);
    void (*_timer_done)(void *);
    void (*_timer_setfn)(void *, void *, void *);
    void (*_timer_arm_us)(void *, uint32_t, bool);
    void (*_wifi_reset_mac)(void);
    void (*_wifi_clock_enable)(void);
    void (*_wifi_clock_disable)(void);
    void (*_wifi_rtc_enable_iso)(void);
    void (*_wifi_rtc_disable_iso)(void);
    int64_t (*_esp_timer_get_time)(void);
    int (*_nvs_set_i8)(uint32_t, const char *, int8_t);
    int (*_nvs_get_i8)(uint32_t, const char *, int8_t *);
    int (*_nvs_set_u8)(uint32_t, const char *, uint8_t);
    int (*_nvs_get_u8)(uint32_t, const char *, uint8_t *);
    int (*_nvs_set_u16)(uint32_t, const char *, uint16_t);
    int (*_nvs_get_u16)(uint32_t, const char *, uint16_t *);
    int (*_nvs_open)(const char *, unsigned int, uint32_t *);
    void (*_nvs_close)(uint32_t);
    int (*_nvs_commit)(uint32_t);
    int (*_nvs_set_blob)(uint32_t, const char *, const void *, size_t);
    int (*_nvs_get_blob)(uint32_t, const char *, void *, size_t *);
    int (*_nvs_erase_key)(uint32_t, const char *);
    int (*_get_random)(uint8_t *, size_t);
    int (*_get_time)(void *);
    unsigned long (*_random)(void);
    uint32_t (*_slowclk_cal_get)(void);
    void (*_log_write)(unsigned int, const char *, const char *, ...);
    void (*_log_writev)(unsigned int, const char *, const char *, va_list);
    uint32_t (*_log_timestamp)(void);
    void *(*_malloc_internal)(size_t);
    void *(*_realloc_internal)(void *, size_t);
    void *(*_calloc_internal)(size_t, size_t);
    void *(*_zalloc_internal)(size_t);
    void *(*_wifi_malloc)(size_t);
    void *(*_wifi_realloc)(void *, size_t);
    void *(*_wifi_calloc)(size_t, size_t);
    void *(*_wifi_zalloc)(size_t);
    void *(*_wifi_create_queue)(int, int);
    void (*_wifi_delete_queue)(void *);
    int (*_coex_init)(void);
    void (*_coex_deinit)(void);
    int (*_coex_enable)(void);
    void (*_coex_disable)(void);
    uint32_t (*_coex_status_get)(void);
    void (*_coex_condition_set)(uint32_t, bool);
    int (*_coex_wifi_request)(uint32_t, uint32_t, uint32_t);
    int (*_coex_wifi_release)(uint32_t);
    int (*_coex_wifi_channel_set)(uint8_t, uint8_t);
    int (*_coex_event_duration_get)(uint32_t, uint32_t *);
    int (*_coex_pti_get)(uint32_t, uint8_t *);
    void (*_coex_schm_status_bit_clear)(uint32_t, uint32_t);
    void (*_coex_schm_status_bit_set)(uint32_t, uint32_t);
    int (*_coex_schm_interval_set)(int);
    uint32_t (*_coex_schm_interval_get)(void);
    uint8_t (*_coex_schm_curr_period_get)(void);
    void *(*_coex_schm_curr_phase_get)(void);
    int (*_coex_schm_process_restart)(void);
    int (*_coex_schm_register_cb)(int, int (*)(int));
    int (*_coex_register_start_cb)(int (*)(void));
    void (*_regdma_link_set_write_wait_content)(void *, uint32_t, uint32_t);
    void *(*_sleep_retention_find_link_by_id)(int);
    int (*_coex_schm_flexible_period_set)(uint8_t);
    uint8_t (*_coex_schm_flexible_period_get)(void);
    int32_t _magic;
} wifi_osi_funcs_t;

static void *osi_wifi_create_queue_int(int len, int sz) {
    return osi_queue_create((uint32_t)len, (uint32_t)sz);
}

wifi_osi_funcs_t g_wifi_osi_funcs = {
    ._version                                 = 0x00000008,
    ._env_is_chip                             = osi_env_is_chip,
    ._set_intr                                = osi_set_intr,
    ._clear_intr                              = osi_clear_intr,
    ._set_isr                                 = osi_set_isr,
    ._ints_on                                 = osi_ints_on,
    ._ints_off                                = osi_ints_off,
    ._is_from_isr                             = osi_is_from_isr,
    ._spin_lock_create                        = osi_spin_lock_create,
    ._spin_lock_delete                        = osi_spin_lock_delete,
    ._wifi_int_disable                        = osi_wifi_int_disable,
    ._wifi_int_restore                        = osi_wifi_int_restore,
    ._task_yield_from_isr                     = osi_task_yield_from_isr,
    ._semphr_create                           = osi_semphr_create,
    ._semphr_delete                           = osi_semphr_delete,
    ._semphr_take                             = osi_semphr_take,
    ._semphr_give                             = osi_semphr_give,
    ._wifi_thread_semphr_get                  = osi_wifi_thread_semphr_get,
    ._mutex_create                            = osi_mutex_create,
    ._recursive_mutex_create                  = osi_mutex_create,
    ._mutex_delete                            = osi_mutex_delete,
    ._mutex_lock                              = osi_mutex_lock,
    ._mutex_unlock                            = osi_mutex_unlock,
    ._queue_create                            = osi_queue_create,
    ._queue_delete                            = osi_queue_delete,
    ._queue_send                              = osi_queue_send,
    ._queue_send_from_isr                     = osi_queue_send_from_isr,
    ._queue_send_to_back                      = osi_queue_send,
    ._queue_send_to_front                     = osi_queue_send_to_front,
    ._queue_recv                              = osi_queue_recv,
    ._queue_msg_waiting                       = osi_queue_msg_waiting,
    ._event_group_create                      = osi_event_group_create,
    ._event_group_delete                      = osi_event_group_delete,
    ._event_group_set_bits                    = osi_event_group_set_bits,
    ._event_group_clear_bits                  = osi_event_group_clear_bits,
    ._event_group_wait_bits                   = osi_event_group_wait_bits,
    ._task_create_pinned_to_core              = osi_task_create_pinned,
    ._task_create                             = osi_task_create,
    ._task_delete                             = osi_task_delete,
    ._task_delay                              = osi_task_delay,
    ._task_ms_to_tick                         = osi_task_ms_to_tick,
    ._task_get_current_task                   = osi_task_get_current,
    ._task_get_max_priority                   = osi_task_get_max_prio,
    ._malloc                                  = wifi_heap_malloc,
    ._free                                    = wifi_heap_free,
    ._event_post                              = osi_event_post,
    ._get_free_heap_size                      = wifi_heap_free_bytes,
    ._rand                                    = osi_rand,
    ._dport_access_stall_other_cpu_start_wrap = osi_noop,
    ._dport_access_stall_other_cpu_end_wrap   = osi_noop,
    ._wifi_apb80m_request                     = osi_noop,
    ._wifi_apb80m_release                     = osi_noop,
    ._phy_disable                             = osi_noop,
    ._phy_enable                              = osi_wifi_clock_enable,
    ._phy_update_country_info                 = osi_phy_country,
    ._read_mac                                = osi_read_mac,
    ._timer_arm                               = osi_timer_arm,
    ._timer_disarm                            = osi_timer_disarm,
    ._timer_done                              = osi_timer_done,
    ._timer_setfn                             = osi_timer_setfn,
    ._timer_arm_us                            = osi_timer_arm_us,
    ._wifi_reset_mac                          = osi_wifi_reset_mac,
    ._wifi_clock_enable                       = osi_wifi_clock_enable,
    ._wifi_clock_disable                      = osi_wifi_clock_disable,
    ._wifi_rtc_enable_iso                     = osi_noop,
    ._wifi_rtc_disable_iso                    = osi_noop,
    ._esp_timer_get_time                      = bsp_timer_get_us,
    ._nvs_set_i8                              = osi_nvs_err_i8,
    ._nvs_get_i8                              = osi_nvs_get_i8,
    ._nvs_set_u8                              = osi_nvs_err_u8,
    ._nvs_get_u8                              = osi_nvs_get_u8,
    ._nvs_set_u16                             = osi_nvs_err_u16,
    ._nvs_get_u16                             = osi_nvs_get_u16,
    ._nvs_open                                = osi_nvs_not_found_open,
    ._nvs_close                               = osi_nvs_close,
    ._nvs_commit                              = osi_nvs_commit,
    ._nvs_set_blob                            = osi_nvs_set_blob,
    ._nvs_get_blob                            = osi_nvs_get_blob,
    ._nvs_erase_key                           = osi_nvs_erase,
    ._get_random                              = osi_get_random,
    ._get_time                                = osi_get_time,
    ._random                                  = osi_random,
    ._slowclk_cal_get                         = osi_slowclk_cal_get,
    ._log_write                               = osi_log_write,
    ._log_writev                              = osi_log_writev,
    ._log_timestamp                           = osi_log_timestamp,
    ._malloc_internal                         = wifi_heap_malloc,
    ._realloc_internal                        = wifi_heap_realloc,
    ._calloc_internal                         = wifi_heap_calloc,
    ._zalloc_internal                         = osi_zalloc,
    ._wifi_malloc                             = wifi_heap_malloc,
    ._wifi_realloc                            = wifi_heap_realloc,
    ._wifi_calloc                             = wifi_heap_calloc,
    ._wifi_zalloc                             = osi_zalloc,
    ._wifi_create_queue                       = osi_wifi_create_queue_int,
    ._wifi_delete_queue                       = osi_queue_delete,
    ._coex_init                               = osi_ok_int,
    ._coex_deinit                             = osi_noop,
    ._coex_enable                             = osi_ok_int,
    ._coex_disable                            = osi_noop,
    ._coex_status_get                         = osi_coex_status,
    ._coex_condition_set                      = osi_coex_cond,
    ._coex_wifi_request                       = osi_coex_req,
    ._coex_wifi_release                       = osi_coex_rel,
    ._coex_wifi_channel_set                   = osi_coex_ch,
    ._coex_event_duration_get                 = osi_coex_dur,
    ._coex_pti_get                            = osi_coex_pti,
    ._coex_schm_status_bit_clear              = osi_coex_schm_u32,
    ._coex_schm_status_bit_set                = osi_coex_schm_u32,
    ._coex_schm_interval_set                  = osi_coex_schm_int,
    ._coex_schm_interval_get                  = osi_coex_schm_get32,
    ._coex_schm_curr_period_get               = osi_coex_schm_get8,
    ._coex_schm_curr_phase_get                = osi_coex_schm_ptr,
    ._coex_schm_process_restart               = osi_ok_int,
    ._coex_schm_register_cb                   = osi_coex_schm_cb,
    ._coex_register_start_cb                  = osi_coex_start_cb,
    ._regdma_link_set_write_wait_content      = osi_regdma_noop,
    ._sleep_retention_find_link_by_id         = osi_sleep_ret_noop,
    ._coex_schm_flexible_period_set           = osi_coex_flex_set,
    ._coex_schm_flexible_period_get           = osi_coex_schm_get8,
    ._magic                                   = (int32_t)0xDEADBEAF,
};

// ---------------------------------------------------------------------------
// High-Level C Bridge to Espressif libnet80211.a (called from bsp/esp32c6/wifi.zc)
// ---------------------------------------------------------------------------
typedef struct {
    uint32_t size;
    uint32_t version;
    void    *funcs[27];
} wpa_crypto_funcs_compat_t;

typedef struct {
    wifi_osi_funcs_t         *osi_funcs;
    wpa_crypto_funcs_compat_t wpa_crypto_funcs;
    int                       static_rx_buf_num;
    int                       dynamic_rx_buf_num;
    int                       tx_buf_type;
    int                       static_tx_buf_num;
    int                       dynamic_tx_buf_num;
    int                       rx_mgmt_buf_type;
    int                       rx_mgmt_buf_num;
    int                       cache_tx_buf_num;
    int                       csi_enable;
    int                       ampdu_rx_enable;
    int                       ampdu_tx_enable;
    int                       amsdu_tx_enable;
    int                       nvs_enable;
    int                       nano_enable;
    int                       rx_ba_win;
    int                       wifi_task_core_id;
    int                       beacon_max_len;
    int                       mgmt_sbuf_num;
    uint64_t                  feature_caps;
    bool                      sta_disconnected_pm;
    int                       espnow_max_encrypt_num;
    int                       tx_hetb_queue_num;
    bool                      dump_hesigb_enable;
    int                       magic;
} wifi_init_config_compat_t;

extern int32_t esp_wifi_init_internal(const wifi_init_config_compat_t *cfg);
extern int32_t esp_wifi_set_mode(int32_t mode);
extern int32_t esp_wifi_start(void);
extern int32_t esp_wifi_scan_start(const void *cfg, bool block);
extern int32_t esp_wifi_scan_get_ap_num(uint16_t *number);
extern int32_t esp_wifi_connect_internal(void);
extern int32_t esp_wifi_disconnect_internal(void);

static char g_wifi_ssid[33];

int32_t wifi_drv_init(void) {
    if (g_wifi_state != 0) return 0;
    osi_wifi_clock_enable();
    wifi_init_config_compat_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.osi_funcs          = &g_wifi_osi_funcs;
    cfg.wpa_crypto_funcs.size = sizeof(wpa_crypto_funcs_compat_t);
    cfg.wpa_crypto_funcs.version = 1;
    cfg.static_rx_buf_num  = 6;
    cfg.dynamic_rx_buf_num = 16;
    cfg.tx_buf_type        = 1;
    cfg.dynamic_tx_buf_num = 16;
    cfg.rx_mgmt_buf_num    = 5;
    cfg.ampdu_rx_enable    = 1;
    cfg.ampdu_tx_enable    = 1;
    cfg.rx_ba_win          = 6;
    cfg.beacon_max_len     = 752;
    cfg.mgmt_sbuf_num      = 6;
    cfg.magic              = 0x1F2F3F4F;

    int32_t err = esp_wifi_init_internal(&cfg);
    if (err != 0) return err;
    err = esp_wifi_set_mode(1); // WIFI_MODE_STA = 1
    if (err != 0) return err;
    err = esp_wifi_start();
    if (err == 0) g_wifi_state = 1;
    return err;
}

int32_t wifi_drv_scan(uint16_t *out_count) {
    int32_t err = wifi_drv_init();
    if (err != 0) return err;
    g_wifi_state = 2;
    err = esp_wifi_scan_start(NULL, true);
    if (err == 0 && out_count) {
        esp_wifi_scan_get_ap_num(out_count);
    }
    g_wifi_state = 1;
    return err;
}

int32_t wifi_drv_connect(const char *ssid, const char *pass) {
    (void)pass;
    int32_t err = wifi_drv_init();
    if (err != 0) return err;
    strncpy(g_wifi_ssid, ssid ? ssid : "", 32);
    g_wifi_ssid[32] = '\0';
    g_wifi_state = 3;
    return esp_wifi_connect_internal();
}

int32_t wifi_drv_disconnect(void) {
    if (g_wifi_state == 0) return 0;
    g_wifi_state = 1;
    return esp_wifi_disconnect_internal();
}

int32_t wifi_drv_get_state(void) { return g_wifi_state; }
const char *wifi_drv_get_ssid(void) { return g_wifi_ssid; }
uint32_t wifi_drv_free_heap(void) { return wifi_heap_free_bytes(); }

// ---------------------------------------------------------------------------
// ESP-IDF / Blob Auxiliary Globals & Calibration Constants
// ---------------------------------------------------------------------------
const char *const WIFI_EVENT = "WIFI_EVENT";

int rtc_clk_xtal_freq_get(void) {
    return 40; // ESP32-C6 40 MHz main crystal
}

uint32_t Basic11BGSched_ptr = 0;
uint8_t  g_espnow_user_oui[3] = { 0x18, 0xFE, 0x34 };
uint32_t mesh_sta_auth_expire_time = 0;
const uint8_t regdomain_table[16] = { 0 };
const uint8_t regulatory_data[64] = { 0 };

// FTM (Fine Timing Measurement) PHY compensation constants (unused in STA Phase 1)
const int16_t est_PHY_INIT_FTM_COMP_20_20D_MHZ     = 0;
const int16_t est_PHY_INIT_FTM_COMP_20_20D_MHZ_DIS = 0;
const int16_t est_PHY_INIT_FTM_COMP_20_20U_MHZ     = 0;
const int16_t est_PHY_INIT_FTM_COMP_20_20U_MHZ_DIS = 0;
const int16_t est_PHY_INIT_FTM_COMP_20_40D_MHZ     = 0;
const int16_t est_PHY_INIT_FTM_COMP_20_40D_MHZ_DIS = 0;
const int16_t est_PHY_INIT_FTM_COMP_20_40U_MHZ     = 0;
const int16_t est_PHY_INIT_FTM_COMP_20_40U_MHZ_DIS = 0;
const int16_t est_PHY_INIT_FTM_COMP_40_40D_MHZ     = 0;
const int16_t est_PHY_INIT_FTM_COMP_40_40D_MHZ_DIS = 0;
const int16_t est_PHY_INIT_FTM_COMP_40_40U_MHZ     = 0;
const int16_t est_PHY_INIT_FTM_COMP_40_40U_MHZ_DIS = 0;
const int16_t est_PHY_RESP_FTM_COMP_20_20D_MHZ     = 0;
const int16_t est_PHY_RESP_FTM_COMP_20_20D_MHZ_DIS = 0;
const int16_t est_PHY_RESP_FTM_COMP_20_20U_MHZ     = 0;
const int16_t est_PHY_RESP_FTM_COMP_20_20U_MHZ_DIS = 0;
const int16_t est_PHY_RESP_FTM_COMP_20_40D_MHZ     = 0;
const int16_t est_PHY_RESP_FTM_COMP_20_40D_MHZ_DIS = 0;
const int16_t est_PHY_RESP_FTM_COMP_20_40U_MHZ     = 0;
const int16_t est_PHY_RESP_FTM_COMP_20_40U_MHZ_DIS = 0;
const int16_t est_PHY_RESP_FTM_COMP_40_40D_MHZ     = 0;
const int16_t est_PHY_RESP_FTM_COMP_40_40D_MHZ_DIS = 0;
const int16_t est_PHY_RESP_FTM_COMP_40_40U_MHZ     = 0;
const int16_t est_PHY_RESP_FTM_COMP_40_40U_MHZ_DIS = 0;
