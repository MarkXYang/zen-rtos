#include "rtos.h"
#include "stm32l476.h"

#define STACK_SIZE_WORDS 256

static uint32_t stack_led[STACK_SIZE_WORDS];
static uint32_t stack_sensor[STACK_SIZE_WORDS];
static uint32_t stack_telemetry[STACK_SIZE_WORDS];

static rtos_mutex_t g_sensor_mutex;
static volatile uint32_t g_shared_temperature = 25;

void task_led_blink(void *arg) {
    (void)arg;
    bsp_init_led();
    while (1) {
        bsp_toggle_led();
        rtos_delay(500); // 500 ms blink
    }
}

void task_sensor_poll(void *arg) {
    (void)arg;
    while (1) {
        rtos_mutex_lock(&g_sensor_mutex, 100);
        // Simulate reading sensor (e.g. ADC/I2C)
        g_shared_temperature++;
        if (g_shared_temperature > 45) {
            g_shared_temperature = 20;
        }
        rtos_mutex_unlock(&g_sensor_mutex);

        rtos_delay(200); // 200 ms sampling
    }
}

void task_telemetry_report(void *arg) {
    (void)arg;
    while (1) {
        rtos_mutex_lock(&g_sensor_mutex, 100);
        uint32_t temp = g_shared_temperature;
        (void)temp;
        rtos_mutex_unlock(&g_sensor_mutex);

        // Periodic broadcast every 1 second
        rtos_delay(1000);
    }
}

int main(void) {
    rtos_init();
    rtos_mutex_init(&g_sensor_mutex);

    // Create tasks: Priority 5 (Sensor), Priority 8 (Telemetry), Priority 10 (LED)
    rtos_task_create(task_sensor_poll, NULL, 5, stack_sensor, STACK_SIZE_WORDS, "Sensor");
    rtos_task_create(task_telemetry_report, NULL, 8, stack_telemetry, STACK_SIZE_WORDS, "Telemetry");
    rtos_task_create(task_led_blink, NULL, 10, stack_led, STACK_SIZE_WORDS, "BlinkLED");

    // Start RTOS on STM32L476: 4 MHz default MSI clock, 1000 Hz SysTick (1 ms)
    rtos_start(4000000, 1000);

    while (1) {
        // Unreachable
    }
    return 0;
}
