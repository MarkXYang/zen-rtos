#ifndef ESP32C6_H
#define ESP32C6_H

#include <stdint.h>
#include <stdbool.h>
#include "rtos.h"

// 1. ESP32-C6 Watchdog Registers (Must be unlocked & disabled at boot!)
#define TIMG0_BASE                  (0x60008000UL)
#define TIMG0_WDTCONFIG0_REG        (*(volatile uint32_t *)(TIMG0_BASE + 0x0048UL))
#define TIMG0_WDTWPROTECT_REG       (*(volatile uint32_t *)(TIMG0_BASE + 0x0064UL))

#define LP_WDT_BASE                 (0x600B1C00UL)
#define LP_WDT_CONFIG0_REG          (*(volatile uint32_t *)(LP_WDT_BASE + 0x0000UL))
#define LP_WDT_WPROTECT_REG         (*(volatile uint32_t *)(LP_WDT_BASE + 0x0018UL))
#define LP_WDT_SWD_CONFIG_REG       (*(volatile uint32_t *)(LP_WDT_BASE + 0x001CUL))
#define LP_WDT_SWD_WPROTECT_REG     (*(volatile uint32_t *)(LP_WDT_BASE + 0x0020UL))

// 2. ESP32-C6 CLINT Machine Timer Registers (DR_REG_CLINT_M_BASE = 0x20001800)
#define CLINT_MSIP_REG              (*(volatile uint32_t *)0x20001800UL)
#define CLINT_MTIMECTL_REG          (*(volatile uint32_t *)0x20001804UL)
#define CLINT_MTIME_LO              (*(volatile uint32_t *)0x20001808UL)
#define CLINT_MTIME_HI              (*(volatile uint32_t *)0x2000180CUL)
#define CLINT_MTIMECMP_LO           (*(volatile uint32_t *)0x20001810UL)
#define CLINT_MTIMECMP_HI           (*(volatile uint32_t *)0x20001814UL)

// 3. ESP32-C6 Built-in USB-Serial/JTAG Console Registers (/dev/ttyACM1)
#define USB_DEVICE_BASE             (0x6000F000UL)
#define USB_DEVICE_EP1_REG          (*(volatile uint32_t *)(USB_DEVICE_BASE + 0x0000UL))
#define USB_DEVICE_EP1_CONF_REG     (*(volatile uint32_t *)(USB_DEVICE_BASE + 0x0004UL))
#define USB_EP1_WR_DONE             (1UL << 0)
#define USB_EP1_IN_DATA_FREE        (1UL << 1)
#define USB_EP1_OUT_DATA_AVAIL      (1UL << 2)

// Also mirror output to UART0 (0x60000000)
#define UART0_FIFO_REG              (*(volatile uint32_t *)0x60000000UL)

// 4. ESP32-C6 GPIO & IO_MUX Registers (GPIO8 = Onboard WS2812B RGB LED on ESP32-C6-Pico)
#define IO_MUX_BASE                 (0x60090000UL)
#define IO_MUX_GPIO8_REG            (*(volatile uint32_t *)(IO_MUX_BASE + 0x0024UL))
#define GPIO_BASE                   (0x60091000UL)
#define GPIO_OUT_REG                (*(volatile uint32_t *)(GPIO_BASE + 0x0004UL))
#define GPIO_OUT_W1TS_REG           (*(volatile uint32_t *)(GPIO_BASE + 0x0008UL))
#define GPIO_OUT_W1TC_REG           (*(volatile uint32_t *)(GPIO_BASE + 0x000CUL))
#define GPIO_ENABLE_W1TS_REG        (*(volatile uint32_t *)(GPIO_BASE + 0x0024UL))
#define GPIO_FUNC8_OUT_SEL_CFG_REG  (*(volatile uint32_t *)(GPIO_BASE + 0x0574UL))
#define LED_GPIO_PIN                8U

extern volatile rtos_fault_info_t g_fault_info;

static uint32_t g_mtimer_step = 160000U;
static volatile bool g_led_state = false;
static volatile uint32_t g_led_color_grb = 0x002000UL; // Default green in 0xRRGGBB order (R=0x00, G=0x20, B=0x00)
static char     g_shell_cmd_buf[64];
static volatile uint32_t g_shell_cmd_len = 0;

// Exported for bsp/esp32c6/startup.s (call bsp_disable_watchdogs)
__attribute__((used)) void bsp_disable_watchdogs(void) {
    // Unlock & disable Timer Group 0 WDT
    TIMG0_WDTWPROTECT_REG = 0x50D83AA1UL;
    TIMG0_WDTCONFIG0_REG  = 0;
    // Unlock & disable RTC / LP WDT
    LP_WDT_WPROTECT_REG   = 0x50D83AA1UL;
    LP_WDT_CONFIG0_REG    = 0;
    // Unlock & auto-feed Super WDT
    LP_WDT_SWD_WPROTECT_REG = 0x50D83AA1UL;
    LP_WDT_SWD_CONFIG_REG  |= (1UL << 18); // SWD_AUTO_FEED_EN
}

// Exported for arch/riscv/rv32/context.s (call bsp_ack_mtimer)
__attribute__((used)) void bsp_ack_mtimer(void) {
    uint64_t now = ((uint64_t)CLINT_MTIME_HI << 32) | CLINT_MTIME_LO;
    uint64_t next = now + g_mtimer_step;
    CLINT_MTIMECMP_HI = 0xFFFFFFFFUL;
    CLINT_MTIMECMP_LO = (uint32_t)(next & 0xFFFFFFFFUL);
    CLINT_MTIMECMP_HI = (uint32_t)(next >> 32);
}

// Exported for kernel/rtos.c (call bsp_start_mtimer)
__attribute__((used)) void bsp_start_mtimer(uint32_t cpu_freq_hz, uint32_t tick_freq_hz) {
    (void)cpu_freq_hz;
    // Enable CLINT MTIME counter (MTCE = bit 0) and timer interrupt (MTIE = bit 1)
    CLINT_MTIMECTL_REG |= 0x03UL;
    // ESP32-C6 HP RISC-V core runs at 160 MHz; 160,000 cycles = 1.000 ms (1000 Hz)
    g_mtimer_step = 160000U / (tick_freq_hz / 1000U);
    bsp_ack_mtimer();
}

// Bare-metal 800 kHz WS2812B-0807 RGB NeoPixel driver on GPIO8 (160 MHz RISC-V core)
// WS2812B-0807 strict timing:
//   '0' bit: T0H = 220..380 ns (~250 ns), T0L = 580..1000 ns (~850 ns)
//   '1' bit: T1H = 580..1000 ns (~750 ns), T1L = 580..1000 ns (~650 ns)
//   Reset/Latch: > 80 us LOW
static inline void bsp_ws2812_write_grb(uint32_t grb) {
    const uint32_t pin_mask = (1UL << LED_GPIO_PIN);
    uint32_t mstatus;
    __asm volatile ("csrrci %0, mstatus, 0x8" : "=r"(mstatus) : : "memory");

    // Ensure GPIO8 is low for >100 us before frame
    GPIO_OUT_W1TC_REG = pin_mask;
    uint32_t t_rst = CLINT_MTIME_LO;
    while ((CLINT_MTIME_LO - t_rst) < 16000U) {}

    for (int bit = 23; bit >= 0; bit--) {
        if ((grb >> bit) & 1U) {
            // '1' bit: ~750 ns HIGH, ~650 ns LOW
            uint32_t t0 = CLINT_MTIME_LO;
            GPIO_OUT_W1TS_REG = pin_mask;
            while ((CLINT_MTIME_LO - t0) < 110U) {}
            GPIO_OUT_W1TC_REG = pin_mask;
            t0 = CLINT_MTIME_LO;
            while ((CLINT_MTIME_LO - t0) < 105U) {}
        } else {
            // '0' bit: ~250 ns HIGH (24 nops at 160 MHz), ~850 ns LOW
            GPIO_OUT_W1TS_REG = pin_mask;
            __asm volatile (
                ".rept 24\n\t"
                "nop\n\t"
                ".endr\n\t"
                : : : "memory"
            );
            GPIO_OUT_W1TC_REG = pin_mask;
            uint32_t t0 = CLINT_MTIME_LO;
            while ((CLINT_MTIME_LO - t0) < 135U) {}
        }
    }

    // Hold low for >100 us after frame so WS2812B-0807 latches the 24-bit color
    t_rst = CLINT_MTIME_LO;
    while ((CLINT_MTIME_LO - t_rst) < 16000U) {}

    if (mstatus & 0x8U) {
        __asm volatile ("csrsi mstatus, 0x8" : : : "memory");
    }
}

static inline void bsp_init_led(void) {
    // Route GPIO8 pad to simple GPIO output:
    // - IO_MUX_GPIO8_REG: MCU_SEL=1 (bit 12), FUN_DRV=2 (bit 10)
    // - GPIO_FUNC8_OUT_SEL_CFG_REG: OEN_SEL=1 (bit 9) | SIG_GPIO_OUT_IDX=128 (0x80)
    IO_MUX_GPIO8_REG = (1UL << 12) | (2UL << 10);
    GPIO_FUNC8_OUT_SEL_CFG_REG = (1UL << 9) | 0x80UL;
    GPIO_OUT_W1TC_REG = (1UL << LED_GPIO_PIN);
    GPIO_ENABLE_W1TS_REG = (1UL << LED_GPIO_PIN);
    bsp_ws2812_write_grb(0x000000UL);
}

static inline void bsp_set_led(bool on) {
    g_led_state = on;
    bsp_ws2812_write_grb(on ? g_led_color_grb : 0x000000UL);
}

static inline void bsp_toggle_led(void) {
    bsp_set_led(!g_led_state);
}

static inline bool bsp_get_led(void) {
    return g_led_state;
}

static inline void bsp_init_uart(uint32_t _cpu_freq_hz, uint32_t _baud) {
    (void)_cpu_freq_hz;
    (void)_baud; // USB-Serial/JTAG is pre-initialized by ESP32-C6 ROM
}

static inline void bsp_usb_write_byte(uint8_t b) {
    for (uint32_t wait = 0; wait < 30000U; wait++) {
        if (USB_DEVICE_EP1_CONF_REG & USB_EP1_IN_DATA_FREE) {
            USB_DEVICE_EP1_REG = b;
            return;
        }
    }
}

static inline void bsp_uart_putc(char c) {
    if (c == '\n') {
        bsp_usb_write_byte((uint8_t)'\r');
    }
    bsp_usb_write_byte((uint8_t)c);
    if (c == '\n') {
        USB_DEVICE_EP1_CONF_REG = USB_EP1_WR_DONE; // Flush USB packet on newline
    }
}

static inline bool bsp_uart_getc(char *out_c) {
    // Bit 2 of EP1_CONF_REG (SERIAL_OUT_EP_DATA_AVAIL) indicates RX byte ready
    if (USB_DEVICE_EP1_CONF_REG & USB_EP1_OUT_DATA_AVAIL) {
        *out_c = (char)(USB_DEVICE_EP1_REG & 0xFFU);
        return true;
    }
    return false;
}

static inline void bsp_uart_puts(const char *s) {
    if (!s) return;
    while (*s) {
        bsp_uart_putc(*s++);
    }
    USB_DEVICE_EP1_CONF_REG = USB_EP1_WR_DONE;
}

static inline void bsp_uart_putdec(uint32_t val) {
    char buf[12];
    int i = 0;
    if (val == 0) {
        bsp_uart_putc('0');
        return;
    }
    while (val > 0 && i < 11) {
        buf[i++] = (char)('0' + (val % 10U));
        val /= 10U;
    }
    while (i > 0) {
        bsp_uart_putc(buf[--i]);
    }
}

static inline void bsp_uart_puthex(uint32_t val) {
    const char hex[] = "0123456789ABCDEF";
    bsp_uart_puts("0x");
    for (int shift = 28; shift >= 0; shift -= 4) {
        bsp_uart_putc(hex[(val >> shift) & 0xFU]);
    }
}

static inline const char *bsp_task_state_str(task_state_t st) {
    switch (st) {
        case TASK_STATE_READY:     return "READY  ";
        case TASK_STATE_BLOCKED:   return "BLOCKED";
        case TASK_STATE_SUSPENDED: return "SUSPEND";
        default:                   return "UNKNOWN";
    }
}

static inline const char *bsp_trace_type_str(uint8_t type) {
    switch ((rtos_trace_type_t)type) {
        case TRACE_TASK_CREATE:     return "TASK_CREATE ";
        case TRACE_TASK_SWITCH:     return "TASK_SWITCH ";
        case TRACE_TASK_DELAY:      return "TASK_DELAY  ";
        case TRACE_TASK_WAKE:       return "TASK_WAKE   ";
        case TRACE_SEM_WAIT:        return "SEM_WAIT    ";
        case TRACE_SEM_SIGNAL:      return "SEM_SIGNAL  ";
        case TRACE_MUTEX_LOCK:      return "MUTEX_LOCK  ";
        case TRACE_MUTEX_UNLOCK:    return "MUTEX_UNLOCK";
        case TRACE_MUTEX_PI_BOOST:  return "MUTEX_PI_BST";
        case TRACE_STACK_OVERFLOW:  return "STACK_OVFLW ";
        case TRACE_FAULT:           return "HARD_FAULT  ";
        case TRACE_ASSERT_FAIL:     return "ASSERT_FAIL ";
        case TRACE_QUEUE_SEND:      return "QUEUE_SEND  ";
        case TRACE_QUEUE_RECV:      return "QUEUE_RECV  ";
        default:                    return "NONE        ";
    }
}

static inline void bsp_debug_dump_status_table(uint32_t shared_temp) {
    rtos_stats_t stats;
    rtos_get_stats(&stats);

    bsp_uart_puts("\n=== [ZenRTOS ESP32-C6 Telemetry] Tick: ");
    bsp_uart_putdec(stats.uptime_ticks);
    bsp_uart_puts(" ms | CPU Load: ");
    bsp_uart_putdec(stats.cpu_load_percent);
    bsp_uart_puts("% | CtxSw: ");
    bsp_uart_putdec(stats.total_ctx_switches);
    bsp_uart_puts(" | Temp: ");
    bsp_uart_putdec(shared_temp);
    bsp_uart_puts("C ===\n");
    bsp_uart_puts("  ID  Name        State    Prio  RunTicks  CtxSw  StackFree\n");
    bsp_uart_puts("  --  ----------  -------  ----  --------  -----  ---------\n");

    for (uint32_t id = 1; id <= stats.task_count; id++) {
        const tcb_t *t = rtos_get_task_by_id(id);
        if (!t) continue;
        uint32_t free_words = rtos_task_stack_free_words(id);
        bsp_uart_puts("  #");
        bsp_uart_putdec(t->id);
        bsp_uart_puts("  ");
        bsp_uart_puts(t->name);
        int len = 0;
        const char *p = t->name;
        while (*p++) len++;
        while (len++ < 12) bsp_uart_putc(' ');

        bsp_uart_puts(bsp_task_state_str(t->state));
        bsp_uart_puts("  ");
        bsp_uart_putdec(t->current_priority);
        if (t->current_priority < 10) bsp_uart_putc(' ');
        bsp_uart_puts("    ");
        bsp_uart_putdec(t->run_ticks);
        bsp_uart_puts("\t   ");
        bsp_uart_putdec(t->ctx_switches);
        bsp_uart_puts("\t  ");
        bsp_uart_putdec(free_words * 4U);
        bsp_uart_puts("/");
        bsp_uart_putdec(t->stack_size * 4U);
        bsp_uart_puts("B\n");
    }
}

static inline void bsp_debug_dump_status(uint32_t shared_temp) {
    if (g_shell_cmd_len > 0) {
        return;
    }
    bsp_debug_dump_status_table(shared_temp);
    bsp_uart_puts("zensh> ");
}

static inline char bsp_tolower(char c) {
    if (c >= 'A' && c <= 'Z') return (char)(c + ('a' - 'A'));
    return c;
}

static inline bool bsp_streq(const char *a, const char *b) {
    while (*a && *b) {
        if (bsp_tolower(*a) != bsp_tolower(*b)) return false;
        a++;
        b++;
    }
    return (*a == '\0' && *b == '\0');
}

static inline void bsp_shell_prompt(void) {
    bsp_uart_puts("zensh> ");
}

static inline void bsp_shell_banner(void) {
    bsp_uart_puts("\n========================================================\n");
    bsp_uart_puts("  ZenRTOS Interactive Shell (zensh v1.2) - ESP32-C6-Pico\n");
    bsp_uart_puts("  Type 'help' for available RTOS inspection commands.   \n");
    bsp_uart_puts("  Type 'log off' to stop periodic telemetry printout.   \n");
    bsp_uart_puts("========================================================\n");
    bsp_shell_prompt();
}

static inline void bsp_shell_exec(const char *raw_cmd, rtos_queue_t *sensor_q,
                                  uint32_t latest_temp, volatile uint32_t *led_auto_blink,
                                  volatile uint32_t *telemetry_log_on) {
    const char *cmd = raw_cmd;
    while (*cmd == ' ' || *cmd == '\t') {
        cmd++;
    }
    if (cmd[0] == '\0') {
        return;
    }

    if (bsp_streq(cmd, "help") || bsp_streq(cmd, "?")) {
        bsp_uart_puts("Available ZenRTOS Shell Commands:\n");
        bsp_uart_puts("  help            - Show this command reference\n");
        bsp_uart_puts("  ps / top        - List all tasks, priorities, CPU load & stack watermarks\n");
        bsp_uart_puts("  trace           - Dump recent kernel trace ring buffer events\n");
        bsp_uart_puts("  sensor / temp   - Read latest sensor temperature & IPC queue count\n");
        bsp_uart_puts("  mem / stack     - Check stack canary (0xDEADBEEF) & high-water marks\n");
        bsp_uart_puts("  led on|off      - Turn GPIO8 WS2812 RGB LED ON or OFF (manual override)\n");
        bsp_uart_puts("  led toggle      - Toggle GPIO8 WS2812 RGB LED state\n");
        bsp_uart_puts("  led red|green|blue - Set WS2812 RGB LED color\n");
        bsp_uart_puts("  led blink       - Resume automatic 1 Hz RTOS LED blinking task\n");
        bsp_uart_puts("  log on|off      - Enable/disable periodic telemetry console stream\n");
        bsp_uart_puts("  fault           - Show RISC-V post-mortem exception CSR record\n");
        bsp_uart_puts("  uptime          - Show system tick counter and uptime\n");
        bsp_uart_puts("  clear           - Clear terminal screen\n");
    } else if (bsp_streq(cmd, "ps") || bsp_streq(cmd, "top")) {
        bsp_debug_dump_status_table(latest_temp);
    } else if (bsp_streq(cmd, "trace")) {
        const rtos_trace_event_t *buf = NULL;
        uint32_t head = 0;
        uint32_t count = rtos_trace_get_events(&buf, &head);
        uint32_t show = count > 12 ? 12 : count;
        bsp_uart_puts("=== Recent Kernel Trace Events (showing last ");
        bsp_uart_putdec(show);
        bsp_uart_puts(" of ");
        bsp_uart_putdec(count);
        bsp_uart_puts(") ===\n");
        for (uint32_t i = 0; i < show; i++) {
            uint32_t idx = (head + RTOS_TRACE_BUFFER_SIZE - show + i) % RTOS_TRACE_BUFFER_SIZE;
            bsp_uart_puts("  [");
            bsp_uart_putdec(buf[idx].tick);
            bsp_uart_puts(" ms] ");
            bsp_uart_puts(bsp_trace_type_str(buf[idx].type));
            bsp_uart_puts(" Task=#");
            bsp_uart_putdec(buf[idx].task_id);
            bsp_uart_puts(" arg1=");
            bsp_uart_putdec(buf[idx].arg1);
            bsp_uart_puts(" arg2=");
            bsp_uart_putdec(buf[idx].arg2);
            bsp_uart_puts("\n");
        }
    } else if (bsp_streq(cmd, "sensor") || bsp_streq(cmd, "temp") || bsp_streq(cmd, "queue") || bsp_streq(cmd, "queues")) {
        bsp_uart_puts("Sensor Temperature : ");
        bsp_uart_putdec(latest_temp);
        bsp_uart_puts(" C\n");
        bsp_uart_puts("IPC Queue Pending  : ");
        bsp_uart_putdec(rtos_queue_count(sensor_q));
        bsp_uart_puts(" / ");
        bsp_uart_putdec(sensor_q ? sensor_q->capacity : 0);
        bsp_uart_puts(" msgs\n");
        bsp_uart_puts("Onboard RGB (GPIO8): ");
        bsp_uart_puts(bsp_get_led() ? "ON" : "OFF");
        bsp_uart_puts((*led_auto_blink) ? " (Auto-Blink 1Hz)\n" : " (Manual Override)\n");
    } else if (bsp_streq(cmd, "mem") || bsp_streq(cmd, "stack")) {
        uint32_t ovf = rtos_check_stack_overflow();
        bsp_uart_puts("Stack Canary Check (0xDEADBEEF): ");
        if (ovf == 0) {
            bsp_uart_puts("PASS (All task stacks intact)\n");
        } else {
            bsp_uart_puts("FAIL! Overflow in Task #");
            bsp_uart_putdec(ovf);
            bsp_uart_puts("\n");
        }
    } else if (bsp_streq(cmd, "led on") || bsp_streq(cmd, "ledon")) {
        *led_auto_blink = 0;
        bsp_set_led(true);
        bsp_uart_puts("WS2812 RGB LED (GPIO8) set to ON.\n");
    } else if (bsp_streq(cmd, "led off") || bsp_streq(cmd, "ledoff")) {
        *led_auto_blink = 0;
        bsp_set_led(false);
        bsp_uart_puts("WS2812 RGB LED (GPIO8) set to OFF.\n");
    } else if (bsp_streq(cmd, "led toggle") || bsp_streq(cmd, "ledtoggle")) {
        *led_auto_blink = 0;
        bsp_toggle_led();
        bsp_uart_puts("WS2812 RGB LED (GPIO8) toggled.\n");
    } else if (bsp_streq(cmd, "led red")) {
        g_led_color_grb = 0x200000UL;
        bsp_set_led(true);
        bsp_uart_puts("WS2812 RGB LED color set to RED.\n");
    } else if (bsp_streq(cmd, "led green")) {
        g_led_color_grb = 0x002000UL;
        bsp_set_led(true);
        bsp_uart_puts("WS2812 RGB LED color set to GREEN.\n");
    } else if (bsp_streq(cmd, "led blue")) {
        g_led_color_grb = 0x000020UL;
        bsp_set_led(true);
        bsp_uart_puts("WS2812 RGB LED color set to BLUE.\n");
    } else if (bsp_streq(cmd, "led blink") || bsp_streq(cmd, "ledblink")) {
        *led_auto_blink = 1;
        bsp_uart_puts("WS2812 RGB LED (GPIO8) returned to automatic 1 Hz RTOS blink.\n");
    } else if (bsp_streq(cmd, "log on") || bsp_streq(cmd, "logon") || bsp_streq(cmd, "log 1")) {
        *telemetry_log_on = 1;
        bsp_uart_puts("Periodic telemetry broadcast ENABLED.\n");
    } else if (bsp_streq(cmd, "log off") || bsp_streq(cmd, "logoff") || bsp_streq(cmd, "log 0") || bsp_streq(cmd, "quiet") || bsp_streq(cmd, "stop")) {
        *telemetry_log_on = 0;
        bsp_uart_puts("Periodic telemetry broadcast DISABLED.\n");
    } else if (bsp_streq(cmd, "fault")) {
        bsp_uart_puts("Fault Status: ");
        bsp_uart_puts(g_fault_info.active ? "ACTIVE FAULT RECORDED\n" : "NONE (Healthy)\n");
        bsp_uart_puts("  MEPC   = "); bsp_uart_puthex(g_fault_info.bfar);
        bsp_uart_puts("  MCAUSE = "); bsp_uart_puthex(g_fault_info.cfsr); bsp_uart_puts("\n");
        bsp_uart_puts("  MTVAL  = "); bsp_uart_puthex(g_fault_info.mmfar);
        bsp_uart_puts("  MSTAT  = "); bsp_uart_puthex(g_fault_info.hfsr); bsp_uart_puts("\n");
    } else if (bsp_streq(cmd, "uptime")) {
        uint32_t ticks = rtos_get_tick();
        bsp_uart_puts("Uptime: ");
        bsp_uart_putdec(ticks / 1000U);
        bsp_uart_putc('.');
        uint32_t ms = ticks % 1000U;
        if (ms < 100U) bsp_uart_putc('0');
        if (ms < 10U)  bsp_uart_putc('0');
        bsp_uart_putdec(ms);
        bsp_uart_puts(" s (");
        bsp_uart_putdec(ticks);
        bsp_uart_puts(" ticks)\n");
    } else if (bsp_streq(cmd, "clear") || bsp_streq(cmd, "cls")) {
        bsp_uart_puts("\033[2J\033[H");
    } else {
        bsp_uart_puts("Unknown command: '");
        bsp_uart_puts(cmd);
        bsp_uart_puts("'. Type 'help' for available commands.\n");
    }
}

static inline void bsp_shell_poll(rtos_queue_t *sensor_q, uint32_t latest_temp,
                                  volatile uint32_t *led_auto_blink,
                                  volatile uint32_t *telemetry_log_on) {
    static uint8_t esc_state = 0;
    static char last_eol = 0;
    char c = 0;

    while (bsp_uart_getc(&c)) {
        if (esc_state == 1) {
            esc_state = (c == '[') ? 2 : 0;
            continue;
        } else if (esc_state == 2) {
            if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '~') {
                esc_state = 0;
            }
            continue;
        }
        if (c == 0x1B) {
            esc_state = 1;
            continue;
        }
        if ((c == '\n' && last_eol == '\r') || (c == '\r' && last_eol == '\n')) {
            last_eol = 0;
            continue;
        }

        if (c == '\r' || c == '\n') {
            last_eol = c;
            bsp_uart_puts("\n");
            g_shell_cmd_buf[g_shell_cmd_len] = '\0';
            while (g_shell_cmd_len > 0 &&
                   (g_shell_cmd_buf[g_shell_cmd_len - 1] == ' ' ||
                    g_shell_cmd_buf[g_shell_cmd_len - 1] == '\t')) {
                g_shell_cmd_buf[--g_shell_cmd_len] = '\0';
            }
            bsp_shell_exec(g_shell_cmd_buf, sensor_q, latest_temp,
                           led_auto_blink, telemetry_log_on);
            g_shell_cmd_len = 0;
            bsp_shell_prompt();
        } else {
            last_eol = 0;
            if (c == '\b' || c == 0x7F) {
                if (g_shell_cmd_len > 0) {
                    g_shell_cmd_len--;
                    bsp_uart_puts("\b \b");
                }
            } else if (c >= 0x20 && c < 0x7F) {
                if (g_shell_cmd_len < sizeof(g_shell_cmd_buf) - 1) {
                    g_shell_cmd_buf[g_shell_cmd_len++] = c;
                    bsp_uart_putc(c);
                    USB_DEVICE_EP1_CONF_REG = USB_EP1_WR_DONE;
                }
            }
        }
    }
}

#endif // ESP32C6_H
