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
#define RTOS_DEFAULT_STACK_SIZE 256          // In 32-bit words (1024 bytes)
#define RTOS_STACK_CANARY       0xDEADBEEFUL // Stack watermark & overflow guard pattern
#define RTOS_TRACE_BUFFER_SIZE  64           // Circular kernel trace buffer entries

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
    uint32_t            id;                  // Task ID (1-based)
    uint32_t            base_priority;       // Assigned base priority (0 = highest, 31 = lowest)
    uint32_t            current_priority;    // Dynamic priority (used in mutex priority inheritance)
    task_state_t        state;               // READY, BLOCKED, etc.
    uint32_t            sleep_ticks;         // Remaining sleep ticks before wake
    const char         *name;                // Debug name
    struct tcb         *next_blocked;        // Linked list pointer for sleep / wait queues
    // Debug & profiling telemetry
    uint32_t           *stack_base;          // Lowest address of stack buffer (canary bottom)
    uint32_t            stack_size;          // Total stack size in 32-bit words
    uint32_t            run_ticks;           // Cumulative SysTick samples spent in this task
    uint32_t            ctx_switches;        // Number of times scheduled into execution
} tcb_t;

// Kernel Trace Event Types
typedef enum {
    TRACE_NONE = 0,
    TRACE_TASK_CREATE,
    TRACE_TASK_SWITCH,
    TRACE_TASK_DELAY,
    TRACE_TASK_WAKE,
    TRACE_SEM_WAIT,
    TRACE_SEM_SIGNAL,
    TRACE_MUTEX_LOCK,
    TRACE_MUTEX_UNLOCK,
    TRACE_MUTEX_PI_BOOST,
    TRACE_STACK_OVERFLOW,
    TRACE_FAULT,
    TRACE_ASSERT_FAIL
} rtos_trace_type_t;

// Circular Trace Buffer Entry (16 bytes aligned)
typedef struct {
    uint32_t            tick;                // System tick timestamp
    uint8_t             type;                // rtos_trace_type_t
    uint8_t             task_id;             // Active or target task ID
    uint16_t            arg1;                // Event-specific metadata (e.g., next task / prio)
    uint32_t            arg2;                // Extended payload (e.g., sleep ticks, fault PC)
} rtos_trace_event_t;

// Cortex-M4 Post-Mortem Fault & Assert Crash Record
typedef struct {
    uint32_t            active;              // 1 = Fault or assertion occurred
    uint32_t            r0;
    uint32_t            r1;
    uint32_t            r2;
    uint32_t            r3;
    uint32_t            r12;
    uint32_t            lr;
    uint32_t            pc;
    uint32_t            xpsr;
    uint32_t            cfsr;                // Configurable Fault Status Register (0xE000ED28)
    uint32_t            hfsr;                // HardFault Status Register (0xE000ED2C)
    uint32_t            mmfar;               // MemManage Fault Address Register (0xE000ED34)
    uint32_t            bfar;                // BusFault Address Register (0xE000ED38)
    uint32_t            offending_task_id;
    const char         *offending_task_name;
    const char         *assert_file;
    uint32_t            assert_line;
} rtos_fault_info_t;

// Global Runtime Statistics Summary
typedef struct {
    uint32_t            uptime_ticks;
    uint32_t            total_ctx_switches;
    uint32_t            task_count;
    uint32_t            idle_ticks;
    uint32_t            cpu_load_percent;    // 0..100% active (non-Idle) CPU utilization
    uint32_t            stack_overflow_task_id;
} rtos_stats_t;

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

// Debug, Profiling & Trace API
uint32_t    rtos_task_stack_free_words(uint32_t task_id);
uint32_t    rtos_check_stack_overflow(void);
void        rtos_get_stats(rtos_stats_t *out_stats);
const tcb_t*rtos_get_task_by_id(uint32_t task_id);
uint32_t    rtos_trace_get_events(const rtos_trace_event_t **out_buf, uint32_t *out_head);
void        rtos_assert_fail(const char *file, uint32_t line);
void        rtos_fault_capture(uint32_t *fault_sp);

#define RTOS_ASSERT(expr) \
    do { if (!(expr)) { rtos_assert_fail(__FILE__, __LINE__); } } while (0)

// Scheduler internal hooks
void        rtos_schedule(void);
void        SysTick_Handler(void);

#ifdef __cplusplus
}
#endif

#endif // RTOS_H
