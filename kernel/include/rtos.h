#ifndef RTOS_H
#define RTOS_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RTOS_MAX_TASKS          16
#define RTOS_MAX_PRIORITIES     32
#define RTOS_DEFAULT_STACK_SIZE 256  // In 32-bit words (1024 bytes)

typedef enum {
    TASK_STATE_READY = 0,
    TASK_STATE_BLOCKED,
    TASK_STATE_SUSPENDED
} task_state_t;

typedef void (*rtos_task_func_t)(void *arg);

// Task Control Block (TCB)
// CRITICAL: sp MUST be the first member (offset 0) for assembly context switch
typedef struct tcb {
    uint32_t           *sp;                  // Current Process Stack Pointer (PSP)
    uint32_t            id;                  // Task ID
    uint32_t            base_priority;       // Assigned base priority (0 = highest, 31 = lowest)
    uint32_t            current_priority;    // Dynamic priority (used in mutex priority inheritance)
    task_state_t        state;               // READY, BLOCKED, etc.
    uint32_t            sleep_ticks;         // Remaining sleep ticks before wake
    const char         *name;                // Debug name
    struct tcb         *next_blocked;        // Linked list pointer for sleep / wait queues
} tcb_t;

// Synchronization: Counting / Binary Semaphore
typedef struct {
    int32_t             count;
    int32_t             max_count;
    tcb_t              *wait_list;
} rtos_sem_t;

// Synchronization: Mutex with Priority Inheritance
typedef struct {
    tcb_t              *owner;
    uint32_t            lock_count;
    uint32_t            original_priority;
    tcb_t              *wait_list;
} rtos_mutex_t;

// Kernel Public API
void        rtos_init(void);
bool        rtos_task_create(rtos_task_func_t func, void *arg, uint32_t priority,
                             uint32_t *stack_buf, uint32_t stack_size_words, const char *name);
void        rtos_start(uint32_t cpu_freq_hz, uint32_t tick_freq_hz);
void        rtos_delay(uint32_t ticks);
void        rtos_yield(void);
uint32_t    rtos_get_tick(void);

// Synchronization API
void        rtos_sem_init(rtos_sem_t *sem, int32_t initial_count, int32_t max_count);
bool        rtos_sem_wait(rtos_sem_t *sem, uint32_t timeout_ticks);
void        rtos_sem_signal(rtos_sem_t *sem);

void        rtos_mutex_init(rtos_mutex_t *mutex);
bool        rtos_mutex_lock(rtos_mutex_t *mutex, uint32_t timeout_ticks);
void        rtos_mutex_unlock(rtos_mutex_t *mutex);

// Scheduler internal hooks
void        rtos_schedule(void);
void        SysTick_Handler(void);

#ifdef __cplusplus
}
#endif

#endif // RTOS_H
