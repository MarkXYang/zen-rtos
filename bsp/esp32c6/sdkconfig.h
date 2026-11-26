// Minimal freestanding compatibility shims so ESP-IDF v5.3.2 Wi-Fi/PHY headers
// compile cleanly under clang --target=riscv32-unknown-elf -ffreestanding.
#ifndef ZENRTOS_ESP32C6_SDKCONFIG_SHIM_H
#define ZENRTOS_ESP32C6_SDKCONFIG_SHIM_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define CONFIG_IDF_TARGET_ESP32C6     1
#define CONFIG_ESP_PHY_MAX_TX_POWER   20

typedef int32_t  esp_err_t;
typedef uint32_t _lock_t;
typedef const char *esp_event_base_t;
typedef struct esp_netif_obj esp_netif_t;

#ifndef BIT
#define BIT(nr) (1UL << (nr))
#endif

#define ESP_OK          0
#define ESP_FAIL        (-1)

#define ESP_EVENT_DECLARE_BASE(id) extern esp_event_base_t const id

#endif // ZENRTOS_ESP32C6_SDKCONFIG_SHIM_H
