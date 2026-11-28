// ZenRTOS ESP32-C6 Wi-Fi OSI Adapter, INTMTX/PLIC IRQ Router, Modem/PHY Shim & Driver Bridge
// Bridges Espressif's prebuilt esp32c6 static archives (libnet80211, libpp, libcore, libcoexist, libphy)
// into ZenRTOS tasks, semaphores, recursive mutexes, software timers, and 192 KB HP SRAM heap.

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdarg.h>
#include "rtos.h"
#include "wifi_os_adapter.h"
#include "esp_wifi.h"
#include "phy.h"
#include "phy_init_data.h"
#include "wifi_wpa2.h"

// ---------------------------------------------------------------------------
// External ZenRTOS, ROM & BSP Helpers
// ---------------------------------------------------------------------------
extern void     bsp_uart_puts(const char *s);
extern int      snprintf(char *buf, size_t cap, const char *fmt, ...);
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
extern void     esp_rom_route_intr_matrix(int cpu_no, uint32_t model_num, uint32_t intr_num);
extern void     esprv_intc_int_set_priority(int rv_int_num, int priority);
extern void     esprv_intc_int_set_threshold(int priority_threshold);
extern void     esprv_intc_int_enable(uint32_t unmask);
extern void     esprv_intc_int_disable(uint32_t mask);
extern void     esprv_intc_int_set_type(int intr_num, int type);
extern void     esp_rom_delay_us(uint32_t us);
extern void     ets_update_cpu_frequency(uint32_t ticks_per_us);
extern void     phy_init_param_set(uint8_t param);
extern void     phy_wifi_enable_set(uint8_t enable);
extern void     phy_bbpll_en_usb(bool en);
extern void     bt_track_pll_cap(void);
extern void     coex_hw_clear_reg(void);
extern volatile uint32_t g_isr_nest_depth;

// ---------------------------------------------------------------------------
// ESP32-C6 Hardware Registers (EFUSE, PLIC_MX, CLINT, MODEM_SYSCON/LPCON, RNG)
// ---------------------------------------------------------------------------
#define EFUSE_RD_MAC_SPI_SYS_0_REG      (*(volatile uint32_t *)0x600B0844UL)
#define EFUSE_RD_MAC_SPI_SYS_1_REG      (*(volatile uint32_t *)0x600B0848UL)

#define PLIC_MX_BASE                    (0x20001000UL)
#define PLIC_MX_ENABLE_REG              (*(volatile uint32_t *)(PLIC_MX_BASE + 0x0000UL))
#define PLIC_MX_TYPE_REG                (*(volatile uint32_t *)(PLIC_MX_BASE + 0x0004UL))
#define PLIC_MX_CLEAR_REG               (*(volatile uint32_t *)(PLIC_MX_BASE + 0x0008UL))
#define PLIC_MX_PRI_REG(num)            (*(volatile uint32_t *)(PLIC_MX_BASE + 0x0010UL + ((num) * 4UL)))
#define PLIC_MX_THRESH_REG              (*(volatile uint32_t *)(PLIC_MX_BASE + 0x0090UL))

#define CLINT_MTIME_LO_REG              (*(volatile uint32_t *)0x20001808UL)
#define CLINT_MTIME_HI_REG              (*(volatile uint32_t *)0x2000180CUL)

#define MODEM_SYSCON_TEST_CONF_REG      (*(volatile uint32_t *)0x600A9800UL)
#define MODEM_SYSCON_CLK_CONF_REG       (*(volatile uint32_t *)0x600A9804UL)
#define MODEM_SYSCON_CLK_FORCE_REG      (*(volatile uint32_t *)0x600A9808UL)
#define MODEM_SYSCON_CLK_POWER_ST_REG   (*(volatile uint32_t *)0x600A980CUL)
#define MODEM_SYSCON_RST_CONF_REG       (*(volatile uint32_t *)0x600A9810UL)
#define MODEM_SYSCON_CLK_CONF1_REG      (*(volatile uint32_t *)0x600A9814UL)
#define MODEM_SYSCON_CLK_FORCE1_REG     (*(volatile uint32_t *)0x600A9818UL)

#define MODEM_LPCON_COEX_LP_CLK_REG     (*(volatile uint32_t *)0x600AF008UL)
#define MODEM_LPCON_WIFI_LP_CLK_REG     (*(volatile uint32_t *)0x600AF00CUL)
#define MODEM_LPCON_I2C_MST_CLK_REG     (*(volatile uint32_t *)0x600AF010UL)
#define MODEM_LPCON_CLK_CONF_REG        (*(volatile uint32_t *)0x600AF018UL)
#define MODEM_LPCON_CLK_FORCE_REG       (*(volatile uint32_t *)0x600AF01CUL)
#define MODEM_LPCON_CLK_POWER_ST_REG    (*(volatile uint32_t *)0x600AF020UL)
#define MODEM_LPCON_RST_CONF_REG        (*(volatile uint32_t *)0x600AF024UL)
#define I2C_MST_ANA_CONF0_REG           (*(volatile uint32_t *)0x600AF818UL)
#define ANA_CONFIG_REG                  (*(volatile uint32_t *)0x600AF81CUL)
#define ANA_CONFIG2_REG                 (*(volatile uint32_t *)0x600AF820UL)

#define PMU_HP_ACTIVE_DIG_POWER_REG     (*(volatile uint32_t *)0x600B0000UL)
#define PMU_HP_ACTIVE_ICG_FUNC_REG      (*(volatile uint32_t *)0x600B0004UL)
#define PMU_HP_ACTIVE_ICG_APB_REG       (*(volatile uint32_t *)0x600B0008UL)
#define PMU_HP_ACTIVE_ICG_MODEM_REG     (*(volatile uint32_t *)0x600B000CUL)
#define PMU_HP_ACTIVE_HP_CK_POWER_REG   (*(volatile uint32_t *)0x600B0014UL)
#define PMU_HP_ACTIVE_BIAS_REG          (*(volatile uint32_t *)0x600B0018UL)
#define PMU_IMM_HP_CK_POWER_REG         (*(volatile uint32_t *)0x600B00CCUL)
#define PMU_IMM_SLEEP_SYSCLK_REG        (*(volatile uint32_t *)0x600B00D0UL)
#define PMU_IMM_HP_FUNC_ICG_REG         (*(volatile uint32_t *)0x600B00D4UL)
#define PMU_IMM_HP_APB_ICG_REG          (*(volatile uint32_t *)0x600B00D8UL)
#define PMU_IMM_MODEM_ICG_REG           (*(volatile uint32_t *)0x600B00DCUL)
#define PMU_IMM_I2C_ISO_REG             (*(volatile uint32_t *)0x600B00E8UL)
#define PMU_POWER_PD_HPWIFI_CNTL_REG    (*(volatile uint32_t *)0x600B0104UL)
#define PMU_RF_PWC_REG                  (*(volatile uint32_t *)0x600B0154UL)
#define PCR_PLL_DIV_CLK_EN_REG          (*(volatile uint32_t *)0x60096114UL)

#define LP_WDEV_RNG_DATA_REG            (*(volatile uint32_t *)0x600B2800UL)

#define HP_APM_FUNC_CTRL_REG            (*(volatile uint32_t *)0x600990C4UL)
#define LP_APM0_FUNC_CTRL_REG           (*(volatile uint32_t *)0x600998C4UL)
#define LP_APM_FUNC_CTRL_REG            (*(volatile uint32_t *)0x600B38C4UL)

// ---------------------------------------------------------------------------
// ESP32-C6 External Interrupt Dispatch (called from arch/riscv/rv32/context.s)
// ---------------------------------------------------------------------------
typedef void (*ext_isr_fn_t)(void *arg);
static ext_isr_fn_t g_ext_isr_fn[32];
static void        *g_ext_isr_arg[32];
static volatile uint32_t g_wifi_irq_count = 0;

void bsp_riscv_ext_irq_dispatch(uint32_t irq_num) {
    g_wifi_irq_count++;
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

// Side-table mapping opaque ETSTimer* pointers to ZenRTOS rtos_timer_t (zero mutation of ETSTimer fields!)
#define MAX_OSI_TIMERS 48
typedef struct {
    void         *ptimer_key;
    rtos_timer_t  rt;
} osi_timer_slot_t;

static osi_timer_slot_t s_timer_slots[MAX_OSI_TIMERS];

static rtos_timer_t *osi_find_or_alloc_timer(void *ptimer, bool alloc_if_missing) {
    if (!ptimer) return NULL;
    for (int i = 0; i < MAX_OSI_TIMERS; i++) {
        if (s_timer_slots[i].ptimer_key == ptimer) {
            return &s_timer_slots[i].rt;
        }
    }
    if (!alloc_if_missing) return NULL;
    for (int i = 0; i < MAX_OSI_TIMERS; i++) {
        if (s_timer_slots[i].ptimer_key == NULL) {
            s_timer_slots[i].ptimer_key = ptimer;
            memset(&s_timer_slots[i].rt, 0, sizeof(rtos_timer_t));
            return &s_timer_slots[i].rt;
        }
    }
    return NULL;
}

static void osi_timer_setfn(void *ptimer, void *pfunction, void *parg) {
    rtos_timer_t *rt = osi_find_or_alloc_timer(ptimer, true);
    if (rt) {
        rtos_timer_init(rt, (rtos_timer_cb_t)pfunction, parg);
    }
}

static void osi_timer_arm(void *ptimer, uint32_t tmout_ms, bool repeat) {
    rtos_timer_t *rt = osi_find_or_alloc_timer(ptimer, false);
    if (rt) {
        rtos_timer_start(rt, tmout_ms ? tmout_ms : 1U, repeat);
    }
}

static void osi_timer_arm_us(void *ptimer, uint32_t us, bool repeat) {
    uint32_t ms = (us + 999U) / 1000U;
    osi_timer_arm(ptimer, ms ? ms : 1U, repeat);
}

static void osi_timer_disarm(void *ptimer) {
    rtos_timer_t *rt = osi_find_or_alloc_timer(ptimer, false);
    if (rt) rtos_timer_stop(rt);
}

static void osi_timer_done(void *ptimer) {
    if (!ptimer) return;
    for (int i = 0; i < MAX_OSI_TIMERS; i++) {
        if (s_timer_slots[i].ptimer_key == ptimer) {
            rtos_timer_stop(&s_timer_slots[i].rt);
            s_timer_slots[i].ptimer_key = NULL;
            break;
        }
    }
}

// ---------------------------------------------------------------------------
// Wi-Fi OSI Function Implementations (Consistent IRQ Remap + PHY RF Init)
// ---------------------------------------------------------------------------
static bool osi_env_is_chip(void) { return true; }

// Remap CLINT-reserved slots (0, 3, 4, 7) deterministically; leave valid slots (e.g. ETS_WMAC_INUM=1) untouched
static uint32_t remap_irq_num(uint32_t n) {
    if (n == 0U) return 9U;
    if (n == 3U) return 10U;
    if (n == 4U) return 11U;
    if (n == 7U) return 12U;
    return n & 31U;
}

static uint32_t remap_irq_mask(uint32_t mask) {
    uint32_t out = 0;
    for (uint32_t b = 0; b < 32U; b++) {
        if (mask & (1UL << b)) {
            out |= (1UL << remap_irq_num(b));
        }
    }
    return out;
}

static void osi_set_intr(int32_t cpu_no, uint32_t intr_source, uint32_t intr_num, int32_t intr_prio) {
    uint32_t hw_num = remap_irq_num(intr_num);
    esp_rom_route_intr_matrix(cpu_no, intr_source, hw_num);
    esprv_intc_int_set_type((int)hw_num, 0); // 0 = INTR_TYPE_LEVEL
    esprv_intc_int_set_priority((int)hw_num, (intr_prio > 0) ? (int)intr_prio : 2);
    esprv_intc_int_set_threshold(0);         // 0 = allow all priorities >= 1 (including WIFI_MAC prio=1!)
}

static void osi_clear_intr(uint32_t intr_source, uint32_t intr_num) {
    (void)intr_source;
    uint32_t hw_num = remap_irq_num(intr_num);
    PLIC_MX_CLEAR_REG |= (1UL << hw_num);
    PLIC_MX_CLEAR_REG &= ~(1UL << hw_num);
}

static void osi_set_isr(int32_t n, void *f, void *arg) {
    if (n < 0) return;
    uint32_t hw_num = remap_irq_num((uint32_t)n);
    g_ext_isr_fn[hw_num]  = (ext_isr_fn_t)f;
    g_ext_isr_arg[hw_num] = arg;
}

static void osi_ints_on(uint32_t mask) {
    uint32_t hw_mask = remap_irq_mask(mask);
    esprv_intc_int_enable(hw_mask);
}

static void osi_ints_off(uint32_t mask) {
    uint32_t hw_mask = remap_irq_mask(mask);
    esprv_intc_int_disable(hw_mask);
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

// Thread-local semaphore per ZenRTOS task ID (0..RTOS_MAX_TASKS) for synchronous Wi-Fi IPC calls
static rtos_sem_t g_wifi_thread_sems[RTOS_MAX_TASKS + 1];
static bool       g_wifi_thread_sem_init[RTOS_MAX_TASKS + 1];

static void *osi_wifi_thread_semphr_get(void) {
    tcb_t *cur = rtos_get_current_task();
    uint32_t idx = (cur && cur->id <= RTOS_MAX_TASKS) ? cur->id : 0U;
    if (!g_wifi_thread_sem_init[idx]) {
        rtos_sem_init(&g_wifi_thread_sems[idx], 0, 1);
        g_wifi_thread_sem_init[idx] = true;
    }
    return &g_wifi_thread_sems[idx];
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

// Critical section hooks called directly by libphy.a
uint32_t phy_enter_critical(void) {
    return rtos_irq_save();
}

void phy_exit_critical(uint32_t level) {
    rtos_irq_restore(level);
}

static bool s_bbpll_inited = false;

static void bsp_bbpll_480m_init_and_pmu_modem_on(void) {
    ets_update_cpu_frequency(160);
    // 0. Disable ESP32-C6 APM access filters so non-CPU DMA masters (Wi-Fi MAC!) can access HP SRAM
    HP_APM_FUNC_CTRL_REG  = 0UL;
    LP_APM0_FUNC_CTRL_REG = 0UL;
    LP_APM_FUNC_CTRL_REG  = 0UL;

    // 1. Power on Wi-Fi digital domain, BBPLL, and latch PMU HP_ACTIVE ICG state (ESP-IDF pmu_init + clk_ll_bbpll_enable)
    PMU_POWER_PD_HPWIFI_CNTL_REG  = 0UL;                        // Clear force PD/ISO/RST on HPWIFI domain
    PMU_HP_ACTIVE_DIG_POWER_REG  &= ~(1UL << 27);               // Clear PD_HP_WIFI_PD_EN
    PMU_HP_ACTIVE_HP_CK_POWER_REG = (1UL << 28) | (1UL << 29) | (1UL << 30); // XPD_BB_I2C | XPD_BBPLL_I2C | XPD_BBPLL
    PMU_HP_ACTIVE_ICG_FUNC_REG    = 0xFFFFFFFFUL;
    PMU_HP_ACTIVE_ICG_APB_REG     = 0xFFFFFFFFUL;
    PMU_HP_ACTIVE_ICG_MODEM_REG   = (2UL << 30);                // PMU_HP_ICG_MODEM_CODE_ACTIVE = 2
    PMU_IMM_HP_CK_POWER_REG       = (1UL << 28) | (1UL << 29) | (1UL << 30); // TIE_HIGH XPD_BB_I2C/BBPLL_I2C/BBPLL
    PMU_IMM_HP_CK_POWER_REG       = (1UL << 25);                // TIE_HIGH_GLOBAL_BBPLL_ICG
    PMU_IMM_HP_FUNC_ICG_REG       = (1UL << 31);                // PMU_UPDATE_DIG_ICG_FUNC_EN
    PMU_IMM_HP_APB_ICG_REG        = (1UL << 31);                // PMU_UPDATE_DIG_ICG_APB_EN
    PMU_IMM_MODEM_ICG_REG         = (1UL << 31);                // PMU_UPDATE_DIG_ICG_MODEM_EN
    PMU_IMM_SLEEP_SYSCLK_REG      = (1UL << 28);                // PMU_UPDATE_DIG_ICG_SWITCH
    PMU_RF_PWC_REG               |= (1UL << 26) | (1UL << 27);  // PMU_PERIF_I2C_RSTB | PMU_XPD_PERIF_I2C
    PCR_PLL_DIV_CLK_EN_REG       |= 0xFFFFFFFFUL;

    // 2. Ungate analog I2C master
    MODEM_LPCON_I2C_MST_CLK_REG  |= (1UL << 0);                 // CLK_I2C_MST_SEL_160M
    MODEM_LPCON_CLK_CONF_REG     |= (1UL << 2);                 // CLK_I2C_MST_EN
    ANA_CONFIG_REG               &= ~((1UL << 18) | (1UL << 17));
    ANA_CONFIG2_REG              |= (1UL << 16);
    s_bbpll_inited = true;
}

static bool s_modem_clk_inited = false;

static void osi_wifi_clock_enable(void) {
    if (s_modem_clk_inited) return;
    s_modem_clk_inited = true;
    bsp_bbpll_480m_init_and_pmu_modem_on();

    // Configure Modem ICG power-state bitmaps (BIT(2)=ACTIVE, BIT(1)=MODEM) per ESP-IDF modem_clock.c
    MODEM_SYSCON_CLK_POWER_ST_REG |= 0x64646400UL;              // APB=6, PERI=4, WIFI=6, BT=4, FE=6, ZB=4
    MODEM_LPCON_CLK_POWER_ST_REG  |= 0x66660000UL;              // LP_APB=6, I2C_MST=6, COEX=6, WIFIPWR=6

    // Enable Wi-Fi BB, Wi-Fi MAC, Wi-Fi APB, and PHY FE clocks (preserve DATA_DUMP_MUX & libphy dynamic bits)
    MODEM_SYSCON_TEST_CONF_REG    |= 1UL;
    MODEM_SYSCON_CLK_CONF1_REG    |= 0x0001E7FFUL;

    // Select ~136 kHz RC_SLOW (OSC_SLOW, div=0) for Wi-Fi & Coex LP clocks and enable LPCON modules
    MODEM_LPCON_WIFI_LP_CLK_REG    = (1UL << 0);
    MODEM_LPCON_COEX_LP_CLK_REG    = (1UL << 0);
    MODEM_LPCON_CLK_CONF_REG      |= (1UL << 0) | (1UL << 1) | (1UL << 2); // WIFIPWR | COEX | I2C_MST
    coex_hw_clear_reg();
}

static void osi_wifi_clock_disable(void) {}

static void osi_wifi_reset_mac(void) {
    // Only reset WIFIMAC (bit 10); NEVER reset WIFIBB (bit 8) which wipes PHY baseband calibration!
    MODEM_SYSCON_RST_CONF_REG |= (1UL << 10);
    MODEM_SYSCON_RST_CONF_REG &= ~(1UL << 10);
}

static bool s_phy_calibrated = false;
static bool s_phy_enabled    = false;
static esp_phy_calibration_data_t *s_phy_cal_data = NULL;

static void osi_phy_enable(void) {
    if (!s_phy_enabled) {
        osi_wifi_clock_enable();
        if (!s_phy_calibrated) {
            phy_init_param_set(1); // SOC_PHY_COMBO_MODULE = 1 on ESP32-C6
            phy_bbpll_en_usb(true);
            if (!s_phy_cal_data) {
                s_phy_cal_data = (esp_phy_calibration_data_t *)wifi_heap_calloc(1, sizeof(esp_phy_calibration_data_t));
            }
            if (s_phy_cal_data) {
                wifi_drv_read_mac(s_phy_cal_data->mac);
                (void)register_chipv7_phy(&phy_init_data, s_phy_cal_data, PHY_RF_CAL_FULL);
                s_phy_calibrated = true;
            }
        } else {
            phy_wakeup_init();
        }
        s_phy_enabled = true;
    }
    bt_track_pll_cap();
    phy_wifi_enable_set(1);
    // Ensure COEX clock stays enabled after PHY init (libphy may clear CLK_COEX_EN)
    MODEM_LPCON_CLK_CONF_REG  |= (1UL << 0) | (1UL << 1) | (1UL << 2);
    MODEM_LPCON_CLK_FORCE_REG |= (1UL << 1);
}

static void osi_phy_disable(void) {
    phy_wifi_enable_set(0);
    if (!s_phy_enabled) return;
    phy_close_rf();
    phy_xpd_tsens();
    s_phy_enabled = false;
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

// Wi-Fi event & L2 Ethernet II bridge tracking (used by zensh> & Phase 2 ZenNet)
typedef esp_err_t (*wifi_rxcb_t)(void *buffer, uint16_t len, void *eb);
typedef void (*wifi_l2_rx_fn_t)(const uint8_t *frame, uint16_t len);
typedef void (*wifi_l2_link_fn_t)(bool up);

extern esp_err_t esp_wifi_internal_reg_rxcb(wifi_interface_t ifx, wifi_rxcb_t fn);
extern esp_err_t esp_wifi_internal_tx(wifi_interface_t ifx, void *buffer, uint16_t len);
extern void      esp_wifi_internal_free_rx_buffer(void *buffer);

static volatile int32_t  g_wifi_state = 0; // 0=UNINIT, 1=IDLE, 2=SCANNING, 3=CONNECTING, 4=CONNECTED
static volatile uint32_t g_l2_rx_frames = 0;
static volatile uint32_t g_l2_tx_frames = 0;
static wifi_l2_rx_fn_t   s_l2_rx_cb   = NULL;
static wifi_l2_link_fn_t s_l2_link_cb = NULL;

static esp_err_t wifi_l2_rx_trampoline(void *buffer, uint16_t len, void *eb) {
    if (buffer && len >= 14U) {
        g_l2_rx_frames++;
        if (s_l2_rx_cb) {
            s_l2_rx_cb((const uint8_t *)buffer, len);
        }
    }
    if (eb) {
        esp_wifi_internal_free_rx_buffer(eb);
    }
    return 0;
}

void wifi_drv_set_l2_callbacks(wifi_l2_rx_fn_t rx_cb, wifi_l2_link_fn_t link_cb) {
    s_l2_rx_cb   = rx_cb;
    s_l2_link_cb = link_cb;
}

int32_t wifi_drv_eth_tx(const uint8_t *frame, uint16_t len) {
    if (!frame || len < 14U || g_wifi_state != 4) return -1;
    esp_err_t err = esp_wifi_internal_tx(WIFI_IF_STA, (void *)frame, len);
    if (err == 0) g_l2_tx_frames++;
    return (int32_t)err;
}

uint32_t wifi_drv_l2_rx_count(void) { return g_l2_rx_frames; }
uint32_t wifi_drv_l2_tx_count(void) { return g_l2_tx_frames; }

static int32_t osi_event_post(const char *base, int32_t id, void *data, size_t len, uint32_t wait) {
    (void)base; (void)wait;
    // WIFI_EVENT_STA_START=2, CONNECTED=4, DISCONNECTED=5, SCAN_DONE=1
    if (id == 2 || id == 1) {
        g_wifi_state = 1;
    } else if (id == 4) {
        g_wifi_state = 4;
        (void)esp_wifi_internal_reg_rxcb(WIFI_IF_STA, wifi_l2_rx_trampoline);
        bsp_uart_puts("[wifi] Connected to AP (L2 link UP)\n");
        if (s_l2_link_cb) s_l2_link_cb(true);
    } else if (id == 5) {
        g_wifi_state = 1;
        (void)esp_wifi_internal_reg_rxcb(WIFI_IF_STA, NULL);
        wifi_wpa2_reset();
        uint8_t reason = 0;
        if (data && len >= sizeof(wifi_event_sta_disconnected_t)) {
            reason = ((const wifi_event_sta_disconnected_t *)data)->reason;
        }
        char msg[64];
        snprintf(msg, sizeof(msg), "[wifi] Disconnected from AP (reason=%u)\n", (unsigned)reason);
        bsp_uart_puts(msg);
        if (s_l2_link_cb) s_l2_link_cb(false);
    }
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
    if (level > 2U) return; // Print Error & Warn only
    char buf[160];
    vsnprintf(buf, sizeof(buf), fmt, args);
    bsp_uart_puts("[");
    bsp_uart_puts(tag ? tag : "wifi");
    bsp_uart_puts("] ");
    bsp_uart_puts(buf);
}
static void osi_log_write(unsigned int level, const char *tag, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    osi_log_writev(level, tag, fmt, ap);
    va_end(ap);
}
static uint32_t osi_log_timestamp(void) { return rtos_get_tick(); }

// Real libcoexist.a integration for ESP32-C6 shared 2.4 GHz RF arbiter
typedef struct {
    int32_t  _version;
    void    (*_task_yield_from_isr)(void);
    void   *(*_semphr_create)(uint32_t max, uint32_t init);
    void    (*_semphr_delete)(void *semphr);
    int32_t (*_semphr_take_from_isr)(void *semphr, void *hptw);
    int32_t (*_semphr_give_from_isr)(void *semphr, void *hptw);
    int32_t (*_semphr_take)(void *semphr, uint32_t block_time_tick);
    int32_t (*_semphr_give)(void *semphr);
    int     (*_is_in_isr)(void);
    void   *(*_malloc_internal)(size_t size);
    void    (*_free)(void *p);
    int64_t (*_esp_timer_get_time)(void);
    bool    (*_env_is_chip)(void);
    void    (*_timer_disarm)(void *timer);
    void    (*_timer_done)(void *ptimer);
    void    (*_timer_setfn)(void *ptimer, void *pfunction, void *parg);
    void    (*_timer_arm_us)(void *ptimer, uint32_t us, bool repeat);
    int     (*_debug_matrix_init)(int evt, int sig, bool rev);
    uint32_t(*_slowclk_cal_get)(void);
    int32_t  _magic;
} coex_adapter_funcs_t;

extern esp_err_t esp_coex_adapter_register(coex_adapter_funcs_t *funcs);
extern esp_err_t coex_pre_init(void);
extern int       coex_init(void);
extern void      coex_deinit(void);
extern int       coex_enable(void);
extern void      coex_disable(void);
extern uint32_t  coex_status_get(uint8_t type);
extern int       coex_wifi_request(uint32_t event, uint32_t latency, uint32_t duration);
extern int       coex_wifi_release(uint32_t event);
extern int       coex_event_duration_get(uint32_t event, uint32_t *duration);
extern int       coex_pti_get(uint32_t event, uint8_t *pti);
extern int       coex_register_start_cb(int (*cb)(void));

static int32_t osi_semphr_take_from_isr(void *s, void *hptw) {
    (void)hptw;
    return rtos_sem_wait((rtos_sem_t *)s, 0) ? 1 : 0;
}
static int32_t osi_semphr_give_from_isr(void *s, void *hptw) {
    bool woken = false;
    rtos_sem_signal_from_isr((rtos_sem_t *)s, &woken);
    if (hptw && woken) *(int32_t *)hptw = 1;
    return 1;
}
static int osi_is_in_isr_int(void) { return g_isr_nest_depth > 0 ? 1 : 0; }
static int osi_coex_debug_matrix_noop(int evt, int sig, bool rev) { (void)evt; (void)sig; (void)rev; return 0; }

static coex_adapter_funcs_t g_coex_adapter_funcs = {
    ._version              = 2,
    ._task_yield_from_isr  = osi_task_yield_from_isr,
    ._semphr_create        = osi_semphr_create,
    ._semphr_delete        = osi_semphr_delete,
    ._semphr_take_from_isr = osi_semphr_take_from_isr,
    ._semphr_give_from_isr = osi_semphr_give_from_isr,
    ._semphr_take          = osi_semphr_take,
    ._semphr_give          = osi_semphr_give,
    ._is_in_isr            = osi_is_in_isr_int,
    ._malloc_internal      = wifi_heap_malloc,
    ._free                 = wifi_heap_free,
    ._esp_timer_get_time   = bsp_timer_get_us,
    ._env_is_chip          = osi_env_is_chip,
    ._timer_disarm         = osi_timer_disarm,
    ._timer_done           = osi_timer_done,
    ._timer_setfn          = osi_timer_setfn,
    ._timer_arm_us         = osi_timer_arm_us,
    ._debug_matrix_init    = osi_coex_debug_matrix_noop,
    ._slowclk_cal_get      = osi_slowclk_cal_get,
    ._magic                = (int32_t)0xDEADBEAF,
};

static uint32_t osi_coex_status(void) { return coex_status_get(0); }
static void osi_coex_cond(uint32_t t, bool d) { (void)t; (void)d; }
static int  osi_coex_ch(uint8_t p, uint8_t s) { (void)p; (void)s; return 0; }
static void osi_coex_schm_u32(uint32_t t, uint32_t s) { (void)t; (void)s; }
static int osi_coex_schm_int(uint32_t v) { (void)v; return 0; }
static int osi_coex_ok_void(void) { return 0; }
static uint32_t osi_coex_schm_get32(void) { return 0; }
static uint8_t  osi_coex_schm_get8(void) { return 0; }
static void    *osi_coex_schm_ptr(void) { return NULL; }
static int  osi_coex_schm_cb(int a, int (*cb)(int)) { (void)a; (void)cb; return 0; }
static void osi_regdma_noop(void *a, uint32_t b, uint32_t c) { (void)a; (void)b; (void)c; }
static void *osi_sleep_ret_noop(int id) { (void)id; return NULL; }
static int  osi_coex_flex_set(uint8_t v) { (void)v; return 0; }
static uint8_t osi_coex_flex_get(void) { return 1; }

// Wrapper matching ESP-IDF wifi_static_queue_t (struct whose first member is the queue handle)
typedef struct {
    void *handle;
} wifi_static_queue_compat_t;

static void *osi_wifi_create_queue_int(int len, int sz) {
    wifi_static_queue_compat_t *sq = (wifi_static_queue_compat_t *)wifi_heap_calloc(1, sizeof(wifi_static_queue_compat_t));
    if (!sq) return NULL;
    sq->handle = osi_queue_create((uint32_t)len, (uint32_t)sz);
    if (!sq->handle) {
        wifi_heap_free(sq);
        return NULL;
    }
    return sq;
}

static void osi_wifi_delete_queue_wrap(void *q) {
    wifi_static_queue_compat_t *sq = (wifi_static_queue_compat_t *)q;
    if (sq) {
        osi_queue_delete(sq->handle);
        wifi_heap_free(sq);
    }
}

wifi_osi_funcs_t g_wifi_osi_funcs = {
    ._version                                 = ESP_WIFI_OS_ADAPTER_VERSION,
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
    ._phy_disable                             = osi_phy_disable,
    ._phy_enable                              = osi_phy_enable,
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
    ._wifi_delete_queue                       = osi_wifi_delete_queue_wrap,
    ._coex_init                               = coex_init,
    ._coex_deinit                             = coex_deinit,
    ._coex_enable                             = coex_enable,
    ._coex_disable                            = coex_disable,
    ._coex_status_get                         = osi_coex_status,
    ._coex_condition_set                      = osi_coex_cond,
    ._coex_wifi_request                       = coex_wifi_request,
    ._coex_wifi_release                       = coex_wifi_release,
    ._coex_wifi_channel_set                   = osi_coex_ch,
    ._coex_event_duration_get                 = coex_event_duration_get,
    ._coex_pti_get                            = coex_pti_get,
    ._coex_schm_status_bit_clear              = osi_coex_schm_u32,
    ._coex_schm_status_bit_set                = osi_coex_schm_u32,
    ._coex_schm_interval_set                  = osi_coex_schm_int,
    ._coex_schm_interval_get                  = osi_coex_schm_get32,
    ._coex_schm_curr_period_get               = osi_coex_schm_get8,
    ._coex_schm_curr_phase_get                = osi_coex_schm_ptr,
    ._coex_schm_process_restart               = osi_coex_ok_void,
    ._coex_schm_register_cb                   = osi_coex_schm_cb,
    ._coex_register_start_cb                  = coex_register_start_cb,
    ._regdma_link_set_write_wait_content      = osi_regdma_noop,
    ._sleep_retention_find_link_by_id         = osi_sleep_ret_noop,
    ._coex_schm_flexible_period_set           = osi_coex_flex_set,
    ._coex_schm_flexible_period_get           = osi_coex_flex_get,
    ._magic                                   = (int32_t)ESP_WIFI_OS_ADAPTER_MAGIC,
};

// ---------------------------------------------------------------------------
// High-Level C Bridge to Espressif libnet80211.a (called from bsp/esp32c6/wifi.zc)
// ---------------------------------------------------------------------------
extern esp_err_t esp_wifi_init_internal(const wifi_init_config_t *config);
extern esp_err_t esp_wifi_internal_set_log_level(int level);
extern esp_err_t esp_wifi_connect_internal(void);
extern esp_err_t esp_wifi_disconnect_internal(void);
extern esp_err_t esp_wifi_register_wpa_cb_internal(const void *cb);

typedef struct {
    void *fn[26];
} wpa_funcs_compat_t;

static bool osi_wpa_true(void) { return true; }
static int  osi_wpa_zero(void) { return 0; }
static void osi_wpa_noop(void) {}

static wpa_funcs_compat_t g_wpa_funcs_compat;

static char g_wifi_ssid[33];

static const char *auth_mode_str(wifi_auth_mode_t m) {
    switch (m) {
        case WIFI_AUTH_OPEN:            return "OPEN    ";
        case WIFI_AUTH_WEP:             return "WEP     ";
        case WIFI_AUTH_WPA_PSK:         return "WPA-PSK ";
        case WIFI_AUTH_WPA2_PSK:        return "WPA2-PSK";
        case WIFI_AUTH_WPA_WPA2_PSK:    return "WPA/WPA2";
        case WIFI_AUTH_WPA3_PSK:        return "WPA3-SAE";
        case WIFI_AUTH_WPA2_WPA3_PSK:   return "WPA2/3  ";
        default:                        return "OTHER   ";
    }
}

int32_t wifi_drv_init(void) {
    if (g_wifi_state != 0) return 0;
    osi_wifi_clock_enable();
    (void)esp_coex_adapter_register(&g_coex_adapter_funcs);
    (void)coex_pre_init();

    wifi_init_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.osi_funcs              = &g_wifi_osi_funcs;
    cfg.wpa_crypto_funcs.size  = sizeof(wpa_crypto_funcs_t);
    cfg.wpa_crypto_funcs.version = 1;
    cfg.static_rx_buf_num      = 6;
    cfg.dynamic_rx_buf_num     = 16;
    cfg.tx_buf_type            = 1;
    cfg.dynamic_tx_buf_num     = 16;
    cfg.rx_mgmt_buf_num        = 5;
    cfg.ampdu_rx_enable        = 1;
    cfg.ampdu_tx_enable        = 1;
    cfg.rx_ba_win              = 6;
    cfg.beacon_max_len         = 752;
    cfg.mgmt_sbuf_num          = 32;
    cfg.tx_hetb_queue_num      = 1;
    cfg.magic                  = WIFI_INIT_CONFIG_MAGIC;

    esp_wifi_internal_set_log_level(2); // WIFI_LOG_WARN
    int32_t err = esp_wifi_init_internal(&cfg);
    if (err != 0) return err;
    memset(&g_wpa_funcs_compat, 0, sizeof(g_wpa_funcs_compat));
    g_wpa_funcs_compat.fn[0]  = (void *)wifi_wpa2_sta_init;        // wpa_sta_init
    g_wpa_funcs_compat.fn[1]  = (void *)wifi_wpa2_sta_deinit;      // wpa_sta_deinit
    g_wpa_funcs_compat.fn[2]  = (void *)wifi_wpa2_sta_connect;     // wpa_sta_connect
    g_wpa_funcs_compat.fn[3]  = (void *)wifi_wpa2_sta_connected;   // wpa_sta_connected_cb
    g_wpa_funcs_compat.fn[4]  = (void *)wifi_wpa2_sta_disconnected;// wpa_sta_disconnected_cb
    g_wpa_funcs_compat.fn[5]  = (void *)wifi_wpa2_sta_rx_eapol;    // wpa_sta_rx_eapol
    g_wpa_funcs_compat.fn[6]  = (void *)wifi_wpa2_sta_in_4way;     // wpa_sta_in_4way_handshake
    g_wpa_funcs_compat.fn[15] = (void *)wifi_wpa2_parse_wpa_ie;    // wpa_parse_wpa_ie
    g_wpa_funcs_compat.fn[17] = (void *)osi_wpa_zero;              // wpa_michael_mic_failure
    g_wpa_funcs_compat.fn[22] = (void *)osi_wpa_noop;              // wpa_config_done
    g_wpa_funcs_compat.fn[25] = (void *)osi_wpa_noop;              // wpa_sta_clear_curr_pmksa
    (void)esp_wifi_register_wpa_cb_internal(&g_wpa_funcs_compat);
    err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != 0) return err;
    err = esp_wifi_start();
    if (err == 0) {
        esp_wifi_set_ps(WIFI_PS_NONE);
        g_wifi_state = 1;
        rtos_delay(50); // Allow async STA_START to settle in wifi task
    }
    return err;
}

int32_t wifi_drv_scan_and_print(uint16_t *out_count) {
    int32_t err = wifi_drv_init();
    if (err != 0) return err;
    g_wifi_state = 2;
    err = esp_wifi_scan_start(NULL, true);
    g_wifi_state = 1;
    if (err != 0) return err;

    uint16_t total = 0;
    (void)esp_wifi_scan_get_ap_num(&total);
    if (out_count) *out_count = total;
    if (total == 0) return 0;

    uint16_t fetch = (total > 16U) ? 16U : total;
    wifi_ap_record_t *aps = (wifi_ap_record_t *)wifi_heap_calloc(fetch, sizeof(wifi_ap_record_t));
    if (!aps) return 0;

    if (esp_wifi_scan_get_ap_records(&fetch, aps) == 0) {
        bsp_uart_puts("  Ch  RSSI    Auth      BSSID              SSID\n");
        rtos_delay(2);
        bsp_uart_puts("  --  ------  --------  -----------------  ------------------------\n");
        rtos_delay(2);
        for (uint16_t i = 0; i < fetch; i++) {
            char line[112];
            snprintf(line, sizeof(line), "  %2u  %3d dBm %s  %02X:%02X:%02X:%02X:%02X:%02X  %s\n",
                     (unsigned)aps[i].primary,
                     (int)aps[i].rssi,
                     auth_mode_str(aps[i].authmode),
                     aps[i].bssid[0], aps[i].bssid[1], aps[i].bssid[2],
                     aps[i].bssid[3], aps[i].bssid[4], aps[i].bssid[5],
                     (const char *)aps[i].ssid);
            bsp_uart_puts(line);
            rtos_delay(2); // Allow 64B USB-Serial/JTAG EP1 FIFO to flush per 1ms USB frame
        }
    }
    wifi_heap_free(aps);
    return 0;
}

int32_t wifi_drv_connect(const char *ssid, const char *pass) {
    int32_t err = wifi_drv_init();
    if (err != 0) return err;

    if (g_wifi_state == 3 || g_wifi_state == 4) {
        (void)esp_wifi_disconnect_internal();
        wifi_wpa2_reset();
        rtos_delay(30);
    }

    wifi_config_t wcfg;
    memset(&wcfg, 0, sizeof(wcfg));
    wcfg.sta.scan_method    = WIFI_ALL_CHANNEL_SCAN;
    wcfg.sta.sort_method    = WIFI_CONNECT_AP_BY_SIGNAL;
    wcfg.sta.threshold.rssi = -127;
    if (ssid) {
        strncpy((char *)wcfg.sta.ssid, ssid, sizeof(wcfg.sta.ssid) - 1);
        strncpy(g_wifi_ssid, ssid, sizeof(g_wifi_ssid) - 1);
        g_wifi_ssid[32] = '\0';
    }
    if (pass && pass[0] != '\0') {
        strncpy((char *)wcfg.sta.password, pass, sizeof(wcfg.sta.password) - 1);
        wcfg.sta.threshold.authmode = WIFI_AUTH_WPA_PSK;
    } else {
        wcfg.sta.threshold.authmode = WIFI_AUTH_OPEN;
    }

    err = esp_wifi_set_config(WIFI_IF_STA, &wcfg);
    if (err != 0) return err;

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

const char *const WIFI_EVENT = "WIFI_EVENT";
uint8_t  g_espnow_user_oui[3] = { 0x18, 0xFE, 0x34 };
uint32_t mesh_sta_auth_expire_time = 0;

int rtc_clk_xtal_freq_get(void) {
    return 40; // ESP32-C6 40 MHz main crystal
}
