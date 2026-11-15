#ifndef STM32L476_H
#define STM32L476_H

#ifdef RTOS_ARCH_RISCV
#include "../esp32c6/esp32c6.h"
#else

#include <stdint.h>
#include <stdbool.h>
#include "rtos.h"

// Peripheral Base Addresses
#define PERIPH_BASE           (0x40000000UL)
#define AHB2PERIPH_BASE       (PERIPH_BASE + 0x08000000UL)
#define APB1PERIPH_BASE       (PERIPH_BASE + 0x00000000UL)
#define APB2PERIPH_BASE       (PERIPH_BASE + 0x00010000UL)

// System Control Block & NVIC
#define SCB_AIRCR             (*(volatile uint32_t *)0xE000ED0CUL)
#define NVIC_ISER1            (*(volatile uint32_t *)0xE000E104UL) // IRQs 32..63 (USART2 = IRQ 38 -> bit 6)

// RCC Registers
#define RCC_BASE              (PERIPH_BASE + 0x00021000UL)
#define RCC_CR                (*(volatile uint32_t *)(RCC_BASE + 0x00))
#define RCC_CFGR              (*(volatile uint32_t *)(RCC_BASE + 0x08))
#define RCC_PLLCFGR           (*(volatile uint32_t *)(RCC_BASE + 0x0C))
#define RCC_AHB2ENR           (*(volatile uint32_t *)(RCC_BASE + 0x4C))
#define RCC_APB1ENR1          (*(volatile uint32_t *)(RCC_BASE + 0x58))

// GPIO Registers (GPIOA = Green LED PA5, USART2 PA2/PA3; GPIOC = User Button PC13)
#define GPIOA_BASE            (AHB2PERIPH_BASE + 0x0000UL)
#define GPIOA_MODER           (*(volatile uint32_t *)(GPIOA_BASE + 0x00))
#define GPIOA_ODR             (*(volatile uint32_t *)(GPIOA_BASE + 0x14))
#define GPIOA_BSRR            (*(volatile uint32_t *)(GPIOA_BASE + 0x18))
#define GPIOA_AFRL            (*(volatile uint32_t *)(GPIOA_BASE + 0x20))

#define GPIOC_BASE            (AHB2PERIPH_BASE + 0x0800UL)
#define GPIOC_MODER           (*(volatile uint32_t *)(GPIOC_BASE + 0x00))
#define GPIOC_IDR             (*(volatile uint32_t *)(GPIOC_BASE + 0x10))

// USART2 Registers (PA2 = TX, PA3 = RX, AF7 -> ST-LINK Virtual COM Port)
#define USART2_BASE           (APB1PERIPH_BASE + 0x4400UL)
#define USART2_CR1            (*(volatile uint32_t *)(USART2_BASE + 0x00))
#define USART2_BRR            (*(volatile uint32_t *)(USART2_BASE + 0x0C))
#define USART2_ISR            (*(volatile uint32_t *)(USART2_BASE + 0x1C))
#define USART2_ICR            (*(volatile uint32_t *)(USART2_BASE + 0x20))
#define USART2_RDR            (*(volatile uint32_t *)(USART2_BASE + 0x24))
#define USART2_TDR            (*(volatile uint32_t *)(USART2_BASE + 0x28))

extern volatile rtos_fault_info_t g_fault_info;

static inline void bsp_init_led(void) {
    // Enable GPIOA clock (bit 0)
    RCC_AHB2ENR |= (1UL << 0);
    // Set PA5 to General Purpose Output (mode 01)
    GPIOA_MODER &= ~(3UL << (5 * 2));
    GPIOA_MODER |=  (1UL << (5 * 2));
}

static inline void bsp_toggle_led(void) {
    GPIOA_ODR ^= (1UL << 5);
}

static inline void bsp_set_led(bool on) {
    if (on) {
        GPIOA_BSRR = (1UL << 5);         // Set PA5 high
    } else {
        GPIOA_BSRR = (1UL << (5 + 16));  // Reset PA5 low
    }
}

static inline bool bsp_get_led(void) {
    return (GPIOA_ODR & (1UL << 5)) != 0;
}

// Interrupt-driven 128-byte UART RX Ring Buffer & Shell State
#define UART_RX_BUF_SIZE      128U
static volatile char          g_uart_rx_buf[UART_RX_BUF_SIZE];
static volatile uint32_t      g_uart_rx_head = 0;
static volatile uint32_t      g_uart_rx_tail = 0;
static char                   g_shell_cmd_buf[64];
static volatile uint32_t      g_shell_cmd_len = 0;

void USART2_IRQHandler(void) {
    uint32_t isr = USART2_ISR;
    if (isr & ((1UL << 3) | (1UL << 2) | (1UL << 1))) {
        USART2_ICR = (1UL << 3) | (1UL << 2) | (1UL << 1);
    }
    while ((USART2_ISR & (1UL << 5)) != 0) {
        char c = (char)(USART2_RDR & 0xFFU);
        uint32_t next = (g_uart_rx_head + 1U) % UART_RX_BUF_SIZE;
        if (next != g_uart_rx_tail) {
            g_uart_rx_buf[g_uart_rx_head] = c;
            g_uart_rx_head = next;
        }
    }
}

// Initialize USART2 on PA2 (TX) and PA3 (RX) connected to ST-LINK VCP
static inline void bsp_init_uart(uint32_t cpu_freq_hz, uint32_t baud) {
    // 1. Enable GPIOA clock (AHB2 bit 0) and USART2 clock (APB1ENR1 bit 17)
    RCC_AHB2ENR  |= (1UL << 0);
    RCC_APB1ENR1 |= (1UL << 17);

    // 2. Configure PA2 and PA3 as Alternate Function (mode 10)
    GPIOA_MODER &= ~((3UL << (2 * 2)) | (3UL << (3 * 2)));
    GPIOA_MODER |=  ((2UL << (2 * 2)) | (2UL << (3 * 2)));

    // 3. Select AF7 (USART2) for PA2 and PA3 in AFRL
    GPIOA_AFRL &= ~((0xFUL << (2 * 4)) | (0xFUL << (3 * 4)));
    GPIOA_AFRL |=  ((0x7UL << (2 * 4)) | (0x7UL << (3 * 4)));

    // 4. Configure baud rate and enable USART2 RXNEIE + TX + RX + UE
    USART2_CR1 = 0;
    USART2_BRR = (cpu_freq_hz + (baud / 2U)) / baud;
    USART2_CR1 = (1UL << 5) | (1UL << 3) | (1UL << 2) | (1UL << 0); // RXNEIE | TE | RE | UE

    // 5. Enable USART2_IRQn (IRQ 38 -> bit 6 in NVIC_ISER1)
    NVIC_ISER1 = (1UL << (38U - 32U));
}

static inline void bsp_uart_putc(char c) {
    // Wait until TXE (Transmit Data Register Empty, bit 7 of ISR) is set
    while ((USART2_ISR & (1UL << 7)) == 0) {}
    USART2_TDR = (uint8_t)c;
}

// Non-blocking UART RX read from interrupt-driven ring buffer
static inline bool bsp_uart_getc(char *out_c) {
    if (g_uart_rx_head != g_uart_rx_tail) {
        *out_c = g_uart_rx_buf[g_uart_rx_tail];
        g_uart_rx_tail = (g_uart_rx_tail + 1U) % UART_RX_BUF_SIZE;
        return true;
    }
    return false;
}

static inline void bsp_uart_puts(const char *s) {
    if (!s) return;
    while (*s) {
        if (*s == '\n') {
            bsp_uart_putc('\r');
        }
        bsp_uart_putc(*s++);
    }
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

    bsp_uart_puts("\n=== [ZenRTOS Debug Telemetry] Tick: ");
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

// Called by background Telemetry task: pauses automatically while user is typing in shell!
static inline void bsp_debug_dump_status(uint32_t shared_temp) {
    if (g_shell_cmd_len > 0) {
        return; // User is actively typing a command; do not interrupt their line!
    }
    bsp_debug_dump_status_table(shared_temp);
    bsp_uart_puts("zensh> ");
}

// -------------------------------------------------------------------------
// Freestanding String Helpers & ZenRTOS Interactive Shell (zensh>)
// -------------------------------------------------------------------------

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
    bsp_uart_puts("  ZenRTOS Interactive Shell (zensh v1.2) - STM32L476RG  \n");
    bsp_uart_puts("  Type 'help' for available RTOS inspection commands.   \n");
    bsp_uart_puts("  Type 'log off' to stop periodic telemetry printout.   \n");
    bsp_uart_puts("========================================================\n");
    bsp_shell_prompt();
}

static inline void bsp_shell_exec(const char *raw_cmd, rtos_queue_t *sensor_q,
                                  uint32_t latest_temp, volatile uint32_t *led_auto_blink,
                                  volatile uint32_t *telemetry_log_on) {
    // Skip leading whitespace
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
        bsp_uart_puts("  led on|off      - Turn PA5 green LED ON or OFF (manual override)\n");
        bsp_uart_puts("  led toggle      - Toggle PA5 green LED state\n");
        bsp_uart_puts("  led blink       - Resume automatic 1 Hz RTOS LED blinking task\n");
        bsp_uart_puts("  log on|off      - Enable/disable periodic telemetry console stream\n");
        bsp_uart_puts("  fault           - Show Cortex-M4 post-mortem HardFault register record\n");
        bsp_uart_puts("  uptime          - Show system tick counter and uptime\n");
        bsp_uart_puts("  clear           - Clear terminal screen\n");
        bsp_uart_puts("  reboot          - Trigger Cortex-M4 software system reset\n");
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
    } else if (bsp_streq(cmd, "sensor") || bsp_streq(cmd, "temp") || bsp_streq(cmd, "queue")) {
        bsp_uart_puts("Sensor Temperature : ");
        bsp_uart_putdec(latest_temp);
        bsp_uart_puts(" C\n");
        bsp_uart_puts("IPC Queue Pending  : ");
        bsp_uart_putdec(rtos_queue_count(sensor_q));
        bsp_uart_puts(" / ");
        bsp_uart_putdec(sensor_q ? sensor_q->capacity : 0);
        bsp_uart_puts(" msgs\n");
        bsp_uart_puts("Green LED (PA5)    : ");
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
        rtos_stats_t stats;
        rtos_get_stats(&stats);
        for (uint32_t id = 1; id <= stats.task_count; id++) {
            const tcb_t *t = rtos_get_task_by_id(id);
            if (!t) continue;
            uint32_t free_b = rtos_task_stack_free_words(id) * 4U;
            uint32_t tot_b  = t->stack_size * 4U;
            uint32_t used_b = tot_b - free_b;
            bsp_uart_puts("  Task #");
            bsp_uart_putdec(id);
            bsp_uart_puts(" [");
            bsp_uart_puts(t->name);
            bsp_uart_puts("]: Used Peak=");
            bsp_uart_putdec(used_b);
            bsp_uart_puts("B, Free High-Watermark=");
            bsp_uart_putdec(free_b);
            bsp_uart_puts("B / ");
            bsp_uart_putdec(tot_b);
            bsp_uart_puts("B\n");
        }
    } else if (bsp_streq(cmd, "led on") || bsp_streq(cmd, "ledon")) {
        *led_auto_blink = 0;
        bsp_set_led(true);
        bsp_uart_puts("LED (PA5) set to ON (manual mode).\n");
    } else if (bsp_streq(cmd, "led off") || bsp_streq(cmd, "ledoff")) {
        *led_auto_blink = 0;
        bsp_set_led(false);
        bsp_uart_puts("LED (PA5) set to OFF (manual mode).\n");
    } else if (bsp_streq(cmd, "led toggle") || bsp_streq(cmd, "ledtoggle")) {
        *led_auto_blink = 0;
        bsp_toggle_led();
        bsp_uart_puts("LED (PA5) toggled -> ");
        bsp_uart_puts(bsp_get_led() ? "ON\n" : "OFF\n");
    } else if (bsp_streq(cmd, "led blink") || bsp_streq(cmd, "ledblink")) {
        *led_auto_blink = 1;
        bsp_uart_puts("LED (PA5) returned to automatic 1 Hz RTOS blink task.\n");
    } else if (bsp_streq(cmd, "log on") || bsp_streq(cmd, "logon") || bsp_streq(cmd, "log 1")) {
        *telemetry_log_on = 1;
        bsp_uart_puts("Periodic telemetry broadcast ENABLED.\n");
    } else if (bsp_streq(cmd, "log off") || bsp_streq(cmd, "logoff") || bsp_streq(cmd, "log 0") || bsp_streq(cmd, "quiet") || bsp_streq(cmd, "stop")) {
        *telemetry_log_on = 0;
        bsp_uart_puts("Periodic telemetry broadcast DISABLED.\n");
    } else if (bsp_streq(cmd, "fault")) {
        bsp_uart_puts("Fault Status: ");
        bsp_uart_puts(g_fault_info.active ? "ACTIVE FAULT RECORDED\n" : "NONE (Healthy)\n");
        bsp_uart_puts("  PC   = "); bsp_uart_puthex(g_fault_info.pc);
        bsp_uart_puts("  LR   = "); bsp_uart_puthex(g_fault_info.lr); bsp_uart_puts("\n");
        bsp_uart_puts("  CFSR = "); bsp_uart_puthex(g_fault_info.cfsr);
        bsp_uart_puts("  HFSR = "); bsp_uart_puthex(g_fault_info.hfsr); bsp_uart_puts("\n");
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
    } else if (bsp_streq(cmd, "reboot") || bsp_streq(cmd, "reset")) {
        bsp_uart_puts("Rebooting STM32L476RG...\n");
        SCB_AIRCR = 0x05FA0004UL;
        while (1) {}
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
        // Filter out ANSI escape sequences (e.g. arrow keys \033[A .. \033[D)
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

        // Handle CRLF (\r\n) without double-executing empty line
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
                }
            }
        }
    }
}

#endif // RTOS_ARCH_RISCV
#endif // STM32L476_H
