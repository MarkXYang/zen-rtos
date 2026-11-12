#ifndef STM32L476_H
#define STM32L476_H

#include <stdint.h>

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

// GPIO Registers (GPIOA = Green LED PA5, GPIOC = User Button PC13)
#define GPIOA_BASE            (AHB2PERIPH_BASE + 0x0000UL)
#define GPIOA_MODER           (*(volatile uint32_t *)(GPIOA_BASE + 0x00))
#define GPIOA_ODR             (*(volatile uint32_t *)(GPIOA_BASE + 0x14))
#define GPIOA_BSRR            (*(volatile uint32_t *)(GPIOA_BASE + 0x18))

#define GPIOC_BASE            (AHB2PERIPH_BASE + 0x0800UL)
#define GPIOC_MODER           (*(volatile uint32_t *)(GPIOC_BASE + 0x00))
#define GPIOC_IDR             (*(volatile uint32_t *)(GPIOC_BASE + 0x10))

// USART2 Registers (PA2 = TX, PA3 = RX)
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

#endif // STM32L476_H
