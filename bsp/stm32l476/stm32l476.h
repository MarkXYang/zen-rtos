#ifndef STM32L476_H
#define STM32L476_H

#include <stdint.h>
#include <stdbool.h>
#include "rtos.h"

// Peripheral Base Addresses
#define PERIPH_BASE           (0x40000000UL)
#define AHB2PERIPH_BASE       (PERIPH_BASE + 0x08000000UL)
#define APB1PERIPH_BASE       (PERIPH_BASE + 0x00000000UL)
#define APB2PERIPH_BASE       (PERIPH_BASE + 0x00010000UL)

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
#define USART2_TDR            (*(volatile uint32_t *)(USART2_BASE + 0x28))

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

    // 4. Configure baud rate and enable USART2 TX + RX + UE
    USART2_CR1 = 0;
    USART2_BRR = (cpu_freq_hz + (baud / 2U)) / baud;
    USART2_CR1 = (1UL << 3) | (1UL << 2) | (1UL << 0); // TE | RE | UE
}

static inline void bsp_uart_putc(char c) {
    // Wait until TXE (Transmit Data Register Empty, bit 7 of ISR) is set
    while ((USART2_ISR & (1UL << 7)) == 0) {}
    USART2_TDR = (uint8_t)c;
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

static inline void bsp_debug_dump_status(uint32_t shared_temp) {
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

    for (uint32_t id = 1; id <= stats.task_count; id++) {
        const tcb_t *t = rtos_get_task_by_id(id);
        if (!t) continue;
        uint32_t free_words = rtos_task_stack_free_words(id);
        bsp_uart_puts("  Task #");
        bsp_uart_putdec(t->id);
        bsp_uart_puts(" [");
        bsp_uart_puts(t->name);
        bsp_uart_puts("] Prio=");
        bsp_uart_putdec(t->current_priority);
        bsp_uart_puts(" RunTicks=");
        bsp_uart_putdec(t->run_ticks);
        bsp_uart_puts(" CtxSw=");
        bsp_uart_putdec(t->ctx_switches);
        bsp_uart_puts(" StackFree=");
        bsp_uart_putdec(free_words * 4U);
        bsp_uart_puts("/");
        bsp_uart_putdec(t->stack_size * 4U);
        bsp_uart_puts("B\n");
    }
}

#endif // STM32L476_H
