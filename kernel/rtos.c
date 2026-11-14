#include "rtos.h"

// Hardware registers for ARM Cortex-M4 (omitted in host test mode)
#ifndef RTOS_HOST_TEST
#define SCB_ICSR          (*(volatile uint32_t *)0xE000ED04)
#define SCB_SHPR3         (*(volatile uint32_t *)0xE000ED20)
#define SCB_CFSR          (*(volatile uint32_t *)0xE000ED28)
#define SCB_HFSR          (*(volatile uint32_t *)0xE000ED2C)
#define SCB_MMFAR         (*(volatile uint32_t *)0xE000ED34)
#define SCB_BFAR          (*(volatile uint32_t *)0xE000ED38)
#define SYST_CSR          (*(volatile uint32_t *)0xE000E010)
#define SYST_RVR          (*(volatile uint32_t *)0xE000E014)
#define SYST_CVR          (*(volatile uint32_t *)0xE000E018)

#define PENDSVSET_BIT     (1UL << 28)

static inline void rtos_enter_critical(void) {
    __asm volatile ("cpsid i" : : : "memory");
}

static inline void rtos_exit_critical(void) {
    __asm volatile ("cpsie i" : : : "memory");
}

static inline void rtos_trigger_pendsv(void) {
    SCB_ICSR = PENDSVSET_BIT;
}
#else
// Host simulation stubs
#define SCB_CFSR          (0U)
#define SCB_HFSR          (0U)
#define SCB_MMFAR         (0U)
#define SCB_BFAR          (0U)

static inline void rtos_enter_critical(void) {}
static inline void rtos_exit_critical(void) {}
static inline void rtos_trigger_pendsv(void) {
    // In host simulation, immediately complete the context switch
    extern volatile tcb_t *current_tcb;
    extern volatile tcb_t *next_tcb;
    if (next_tcb != NULL) {
        current_tcb = next_tcb;
    }
}
#endif

// Global kernel state (exported symbols for OpenOCD / GDB inspection)
volatile tcb_t              *current_tcb = NULL;
volatile tcb_t              *next_tcb = NULL;
volatile rtos_fault_info_t   g_fault_info;
rtos_trace_event_t           g_trace_buffer[RTOS_TRACE_BUFFER_SIZE];
volatile uint32_t            g_trace_head = 0;
volatile uint32_t            g_trace_count = 0;
volatile uint32_t            g_total_ctx_switches = 0;
volatile uint32_t            g_stack_overflow_task_id = 0;

static tcb_t                 g_tasks[RTOS_MAX_TASKS];
static uint32_t              g_task_count = 0;
static uint32_t              g_ready_bitmap = 0;       // Bit N set means priority N has ready task
static volatile uint32_t     g_system_ticks = 0;

// Idle task stack & TCB
#define IDLE_STACK_SIZE 128
static uint32_t              g_idle_stack[IDLE_STACK_SIZE];

static void rtos_trace_record(rtos_trace_type_t type, uint8_t task_id, uint16_t arg1, uint32_t arg2) {
    uint32_t idx = g_trace_head;
    g_trace_buffer[idx].tick = g_system_ticks;
    g_trace_buffer[idx].type = (uint8_t)type;
    g_trace_buffer[idx].task_id = task_id;
    g_trace_buffer[idx].arg1 = arg1;
    g_trace_buffer[idx].arg2 = arg2;
    g_trace_head = (idx + 1) % RTOS_TRACE_BUFFER_SIZE;
    if (g_trace_count < RTOS_TRACE_BUFFER_SIZE) {
        g_trace_count++;
    }
}

static void rtos_idle_task(void *arg) {
    (void)arg;
    while (1) {
#ifndef RTOS_HOST_TEST
        __asm volatile ("wfi");
#endif
    }
}

static void rtos_task_exit_hook(void) {
    // If a task function returns, halt and suspend it
    rtos_enter_critical();
    if (current_tcb) {
        ((tcb_t *)current_tcb)->state = TASK_STATE_SUSPENDED;
        g_ready_bitmap &= ~(1UL << current_tcb->current_priority);
    }
    rtos_exit_critical();
    rtos_schedule();
    while (1) {}
}

void rtos_init(void) {
    g_task_count = 0;
    g_ready_bitmap = 0;
    g_system_ticks = 0;
    g_trace_head = 0;
    g_trace_count = 0;
    g_total_ctx_switches = 0;
    g_stack_overflow_task_id = 0;
    current_tcb = NULL;
    next_tcb = NULL;

    g_fault_info.active = 0;
    g_fault_info.offending_task_id = 0;
    g_fault_info.offending_task_name = NULL;
    g_fault_info.assert_file = NULL;
    g_fault_info.assert_line = 0;

    for (uint32_t i = 0; i < RTOS_TRACE_BUFFER_SIZE; i++) {
        g_trace_buffer[i].tick = 0;
        g_trace_buffer[i].type = TRACE_NONE;
        g_trace_buffer[i].task_id = 0;
        g_trace_buffer[i].arg1 = 0;
        g_trace_buffer[i].arg2 = 0;
    }

    for (uint32_t i = 0; i < RTOS_MAX_TASKS; i++) {
        g_tasks[i].state = TASK_STATE_SUSPENDED;
        g_tasks[i].sp = NULL;
        g_tasks[i].next_blocked = NULL;
        g_tasks[i].stack_base = NULL;
        g_tasks[i].stack_size = 0;
        g_tasks[i].run_ticks = 0;
        g_tasks[i].ctx_switches = 0;
    }

    // Create Idle Task at lowest priority (31) -> Task ID 1
    rtos_task_create(rtos_idle_task, NULL, 31, g_idle_stack, IDLE_STACK_SIZE, "Idle");
}

bool rtos_task_create(rtos_task_func_t func, void *arg, uint32_t priority,
                      uint32_t *stack_buf, uint32_t stack_size_words, const char *name) {
    if (g_task_count >= RTOS_MAX_TASKS || !func || !stack_buf ||
        stack_size_words < 32 || priority >= RTOS_MAX_PRIORITIES) {
        return false;
    }

    // Fill entire stack with canary pattern (0xDEADBEEF) for high-watermark & overflow detection
    for (uint32_t i = 0; i < stack_size_words; i++) {
        stack_buf[i] = RTOS_STACK_CANARY;
    }

    tcb_t *t = &g_tasks[g_task_count++];
    t->id = g_task_count;
    t->base_priority = priority;
    t->current_priority = priority;
    t->state = TASK_STATE_READY;
    t->sleep_ticks = 0;
    t->name = name ? name : "Task";
    t->next_blocked = NULL;
    t->stack_base = stack_buf;
    t->stack_size = stack_size_words;
    t->run_ticks = 0;
    t->ctx_switches = 0;

    // Point to top of stack, aligned to 8 bytes
    uint32_t *sp = &stack_buf[stack_size_words - 1];
    sp = (uint32_t *)((uintptr_t)sp & ~0x7);

    // Build initial Cortex-M4 stack frame
    // 1. Hardware auto-saved registers
    *(--sp) = 0x01000000;                               // xPSR: Default Thumb mode bit (Bit 24)
    *(--sp) = (uint32_t)(uintptr_t)func;                // PC: Task entry point
    *(--sp) = (uint32_t)(uintptr_t)rtos_task_exit_hook; // LR: Clean exit if task returns
    *(--sp) = 0x00000000;                               // R12
    *(--sp) = 0x00000000;                               // R3
    *(--sp) = 0x00000000;                               // R2
    *(--sp) = 0x00000000;                               // R1
    *(--sp) = (uint32_t)(uintptr_t)arg;                 // R0: void *arg parameter

    // 2. Software saved registers (R4 - R11)
    for (int i = 0; i < 8; i++) {
        *(--sp) = 0x00000000;
    }

    t->sp = sp;

    // Mark priority as ready in bitmap
    g_ready_bitmap |= (1UL << priority);

    rtos_trace_record(TRACE_TASK_CREATE, (uint8_t)t->id, (uint16_t)priority, stack_size_words);

    return true;
}

// O(1) Priority Scheduler
void rtos_schedule(void) {
    if (g_ready_bitmap == 0) return;

    // Count Trailing Zeros: finds the lowest bit set (which is highest priority, 0 = top)
    uint32_t highest_prio = (uint32_t)__builtin_ctz(g_ready_bitmap);

    // Find the ready task with this priority
    tcb_t *selected = NULL;
    for (uint32_t i = 0; i < g_task_count; i++) {
        if (g_tasks[i].state == TASK_STATE_READY && g_tasks[i].current_priority == highest_prio) {
            selected = &g_tasks[i];
            break;
        }
    }

    if (selected && selected != current_tcb) {
        uint8_t prev_id = current_tcb ? (uint8_t)current_tcb->id : 0;
        selected->ctx_switches++;
        g_total_ctx_switches++;
        rtos_trace_record(TRACE_TASK_SWITCH, prev_id, (uint16_t)selected->id, selected->current_priority);
        next_tcb = selected;
        rtos_trigger_pendsv();
    }
}

void SysTick_Handler(void) {
    g_system_ticks++;

    // Sample CPU runtime for currently active task & check stack canary
    if (current_tcb) {
        ((tcb_t *)current_tcb)->run_ticks++;
        if (current_tcb->stack_base &&
            (current_tcb->stack_base[0] != RTOS_STACK_CANARY ||
             current_tcb->sp < current_tcb->stack_base)) {
            g_stack_overflow_task_id = current_tcb->id;
            rtos_trace_record(TRACE_STACK_OVERFLOW, (uint8_t)current_tcb->id,
                              (uint16_t)current_tcb->current_priority,
                              (uint32_t)(uintptr_t)current_tcb->sp);
        }
    }

    // Process sleeping/blocked tasks
    for (uint32_t i = 0; i < g_task_count; i++) {
        if (g_tasks[i].state == TASK_STATE_BLOCKED && g_tasks[i].sleep_ticks > 0) {
            if (--g_tasks[i].sleep_ticks == 0) {
                g_tasks[i].state = TASK_STATE_READY;
                g_ready_bitmap |= (1UL << g_tasks[i].current_priority);
                rtos_trace_record(TRACE_TASK_WAKE, (uint8_t)g_tasks[i].id,
                                  (uint16_t)g_tasks[i].current_priority, 0);
            }
        }
    }

    rtos_schedule();
}

void rtos_delay(uint32_t ticks) {
    if (ticks == 0 || !current_tcb) return;

    rtos_enter_critical();
    tcb_t *t = (tcb_t *)current_tcb;
    t->sleep_ticks = ticks;
    t->state = TASK_STATE_BLOCKED;

    rtos_trace_record(TRACE_TASK_DELAY, (uint8_t)t->id, (uint16_t)t->current_priority, ticks);

    // If no other task is ready at this priority, clear bit
    bool another_ready = false;
    for (uint32_t i = 0; i < g_task_count; i++) {
        if (&g_tasks[i] != t && g_tasks[i].state == TASK_STATE_READY &&
            g_tasks[i].current_priority == t->current_priority) {
            another_ready = true;
            break;
        }
    }
    if (!another_ready) {
        g_ready_bitmap &= ~(1UL << t->current_priority);
    }
    rtos_exit_critical();

    rtos_schedule();

#ifndef RTOS_HOST_TEST
    while (t->state == TASK_STATE_BLOCKED) {
        // Spin until context switch occurs
    }
#endif
}

void rtos_yield(void) {
    rtos_trigger_pendsv();
}

uint32_t rtos_get_tick(void) {
    return g_system_ticks;
}

// -------------------------------------------------------------------------
// Synchronization Primitives
// -------------------------------------------------------------------------

void rtos_sem_init(rtos_sem_t *sem, int32_t initial_count, int32_t max_count) {
    if (!sem) return;
    sem->count = initial_count;
    sem->max_count = max_count > 0 ? max_count : 1;
    sem->wait_list = NULL;
}

bool rtos_sem_wait(rtos_sem_t *sem, uint32_t timeout_ticks) {
    if (!sem || !current_tcb) return false;

    rtos_enter_critical();
    if (sem->count > 0) {
        sem->count--;
        rtos_trace_record(TRACE_SEM_WAIT, (uint8_t)current_tcb->id, 1, (uint32_t)sem->count);
        rtos_exit_critical();
        return true;
    }

    // Count is 0 -> block current task
    tcb_t *t = (tcb_t *)current_tcb;
    t->state = TASK_STATE_BLOCKED;
    t->sleep_ticks = timeout_ticks;
    g_ready_bitmap &= ~(1UL << t->current_priority);

    rtos_trace_record(TRACE_SEM_WAIT, (uint8_t)t->id, 0, timeout_ticks);

    // Append to wait list
    t->next_blocked = sem->wait_list;
    sem->wait_list = t;
    rtos_exit_critical();

    rtos_schedule();

    return (t->sleep_ticks > 0 || timeout_ticks == 0);
}

void rtos_sem_signal(rtos_sem_t *sem) {
    if (!sem) return;

    rtos_enter_critical();
    if (sem->wait_list != NULL) {
        // Wake the first blocked task
        tcb_t *woken = sem->wait_list;
        sem->wait_list = woken->next_blocked;
        woken->next_blocked = NULL;
        woken->state = TASK_STATE_READY;
        woken->sleep_ticks = 0;
        g_ready_bitmap |= (1UL << woken->current_priority);
        rtos_trace_record(TRACE_SEM_SIGNAL, (uint8_t)woken->id, 1, 0);
        rtos_exit_critical();
        rtos_schedule();
        return;
    }

    if (sem->count < sem->max_count) {
        sem->count++;
    }
    rtos_trace_record(TRACE_SEM_SIGNAL, current_tcb ? (uint8_t)current_tcb->id : 0, 0, (uint32_t)sem->count);
    rtos_exit_critical();
}

void rtos_mutex_init(rtos_mutex_t *mutex) {
    if (!mutex) return;
    mutex->owner = NULL;
    mutex->lock_count = 0;
    mutex->original_priority = 0;
    mutex->wait_list = NULL;
}

bool rtos_mutex_lock(rtos_mutex_t *mutex, uint32_t timeout_ticks) {
    if (!mutex || !current_tcb) return false;

    rtos_enter_critical();
    tcb_t *curr = (tcb_t *)current_tcb;

    // Recursive lock
    if (mutex->owner == curr) {
        mutex->lock_count++;
        rtos_exit_critical();
        return true;
    }

    if (mutex->owner == NULL) {
        mutex->owner = curr;
        mutex->lock_count = 1;
        mutex->original_priority = curr->current_priority;
        rtos_trace_record(TRACE_MUTEX_LOCK, (uint8_t)curr->id, 1, curr->current_priority);
        rtos_exit_critical();
        return true;
    }

    // Priority Inheritance: If current task priority is higher (lower number), boost owner
    if (curr->current_priority < mutex->owner->current_priority) {
        uint32_t old_prio = mutex->owner->current_priority;
        g_ready_bitmap &= ~(1UL << old_prio);
        mutex->owner->current_priority = curr->current_priority;
        if (mutex->owner->state == TASK_STATE_READY) {
            g_ready_bitmap |= (1UL << mutex->owner->current_priority);
        }
        rtos_trace_record(TRACE_MUTEX_PI_BOOST, (uint8_t)mutex->owner->id,
                          (uint16_t)old_prio, curr->current_priority);
    }

    curr->state = TASK_STATE_BLOCKED;
    curr->sleep_ticks = timeout_ticks;
    g_ready_bitmap &= ~(1UL << curr->current_priority);

    curr->next_blocked = mutex->wait_list;
    mutex->wait_list = curr;
    rtos_trace_record(TRACE_MUTEX_LOCK, (uint8_t)curr->id, 0, timeout_ticks);
    rtos_exit_critical();

    rtos_schedule();
    return true;
}

void rtos_mutex_unlock(rtos_mutex_t *mutex) {
    if (!mutex || !current_tcb || mutex->owner != current_tcb) return;

    rtos_enter_critical();
    mutex->lock_count--;
    if (mutex->lock_count > 0) {
        rtos_exit_critical();
        return;
    }

    // Restore owner's original priority
    tcb_t *prev_owner = mutex->owner;
    if (prev_owner->current_priority != mutex->original_priority) {
        g_ready_bitmap &= ~(1UL << prev_owner->current_priority);
        prev_owner->current_priority = mutex->original_priority;
        if (prev_owner->state == TASK_STATE_READY) {
            g_ready_bitmap |= (1UL << prev_owner->current_priority);
        }
    }
    mutex->owner = NULL;
    rtos_trace_record(TRACE_MUTEX_UNLOCK, (uint8_t)prev_owner->id,
                      (uint16_t)prev_owner->current_priority, 0);

    // Transfer ownership to highest priority waiting task
    if (mutex->wait_list != NULL) {
        tcb_t *new_owner = mutex->wait_list;
        mutex->wait_list = new_owner->next_blocked;
        new_owner->next_blocked = NULL;

        new_owner->state = TASK_STATE_READY;
        new_owner->sleep_ticks = 0;
        g_ready_bitmap |= (1UL << new_owner->current_priority);

        mutex->owner = new_owner;
        mutex->lock_count = 1;
        mutex->original_priority = new_owner->current_priority;
    }

    rtos_exit_critical();
    rtos_schedule();
}

// -------------------------------------------------------------------------
// Debug, Profiling & Trace Implementation
// -------------------------------------------------------------------------

uint32_t rtos_task_stack_free_words(uint32_t task_id) {
    if (task_id == 0 || task_id > g_task_count) return 0;
    const tcb_t *t = &g_tasks[task_id - 1];
    if (!t->stack_base || t->stack_size == 0) return 0;

    uint32_t free_words = 0;
    for (uint32_t i = 0; i < t->stack_size; i++) {
        if (t->stack_base[i] == RTOS_STACK_CANARY) {
            free_words++;
        } else {
            break;
        }
    }
    return free_words;
}

uint32_t rtos_check_stack_overflow(void) {
    for (uint32_t i = 0; i < g_task_count; i++) {
        const tcb_t *t = &g_tasks[i];
        if (t->stack_base) {
            if (t->stack_base[0] != RTOS_STACK_CANARY || t->sp < t->stack_base) {
                g_stack_overflow_task_id = t->id;
                rtos_trace_record(TRACE_STACK_OVERFLOW, (uint8_t)t->id,
                                  (uint16_t)t->current_priority,
                                  (uint32_t)(uintptr_t)t->sp);
                return t->id;
            }
        }
    }
    return 0;
}

const tcb_t *rtos_get_task_by_id(uint32_t task_id) {
    if (task_id == 0 || task_id > g_task_count) return NULL;
    return &g_tasks[task_id - 1];
}

void rtos_get_stats(rtos_stats_t *out_stats) {
    if (!out_stats) return;
    out_stats->uptime_ticks = g_system_ticks;
    out_stats->total_ctx_switches = g_total_ctx_switches;
    out_stats->task_count = g_task_count;
    out_stats->idle_ticks = (g_task_count > 0) ? g_tasks[0].run_ticks : 0;
    out_stats->stack_overflow_task_id = g_stack_overflow_task_id;

    if (g_system_ticks > 0 && g_system_ticks >= out_stats->idle_ticks) {
        uint32_t busy_ticks = g_system_ticks - out_stats->idle_ticks;
        out_stats->cpu_load_percent = (busy_ticks * 100U) / g_system_ticks;
    } else {
        out_stats->cpu_load_percent = 0;
    }
}

uint32_t rtos_trace_get_events(const rtos_trace_event_t **out_buf, uint32_t *out_head) {
    if (out_buf) *out_buf = g_trace_buffer;
    if (out_head) *out_head = g_trace_head;
    return g_trace_count;
}

void rtos_assert_fail(const char *file, uint32_t line) {
    rtos_enter_critical();
    g_fault_info.active = 1;
    g_fault_info.assert_file = file;
    g_fault_info.assert_line = line;
    g_fault_info.offending_task_id = current_tcb ? current_tcb->id : 0;
    g_fault_info.offending_task_name = current_tcb ? current_tcb->name : "Kernel";
    rtos_trace_record(TRACE_ASSERT_FAIL, (uint8_t)g_fault_info.offending_task_id, 0, line);
#ifndef RTOS_HOST_TEST
    while (1) {
        __asm volatile ("nop");
    }
#endif
}

void rtos_fault_capture(uint32_t *fault_sp) {
    g_fault_info.active = 1;
    if (fault_sp) {
        g_fault_info.r0   = fault_sp[0];
        g_fault_info.r1   = fault_sp[1];
        g_fault_info.r2   = fault_sp[2];
        g_fault_info.r3   = fault_sp[3];
        g_fault_info.r12  = fault_sp[4];
        g_fault_info.lr   = fault_sp[5];
        g_fault_info.pc   = fault_sp[6];
        g_fault_info.xpsr = fault_sp[7];
    }
    g_fault_info.cfsr  = SCB_CFSR;
    g_fault_info.hfsr  = SCB_HFSR;
    g_fault_info.mmfar = SCB_MMFAR;
    g_fault_info.bfar  = SCB_BFAR;
    g_fault_info.offending_task_id   = current_tcb ? current_tcb->id : 0;
    g_fault_info.offending_task_name = current_tcb ? current_tcb->name : "ISR/Kernel";

    rtos_trace_record(TRACE_FAULT, (uint8_t)g_fault_info.offending_task_id,
                      (uint16_t)(g_fault_info.cfsr & 0xFFFFU), g_fault_info.pc);

#ifndef RTOS_HOST_TEST
    while (1) {
        __asm volatile ("nop");
    }
#endif
}

#ifndef RTOS_HOST_TEST
extern void rtos_start_first_task(uint32_t *sp);

void rtos_start(uint32_t cpu_freq_hz, uint32_t tick_freq_hz) {
    // Select the highest priority ready task
    uint32_t highest_prio = (uint32_t)__builtin_ctz(g_ready_bitmap);
    for (uint32_t i = 0; i < g_task_count; i++) {
        if (g_tasks[i].current_priority == highest_prio) {
            current_tcb = &g_tasks[i];
            g_tasks[i].ctx_switches = 1;
            g_total_ctx_switches = 1;
            rtos_trace_record(TRACE_TASK_SWITCH, 0, (uint16_t)g_tasks[i].id, highest_prio);
            break;
        }
    }

    // Set PendSV priority to lowest (0xFF)
    SCB_SHPR3 |= (0xFFUL << 16);

    // Setup SysTick
    SYST_RVR = (cpu_freq_hz / tick_freq_hz) - 1;
    SYST_CVR = 0;
    SYST_CSR = (1 << 0) | (1 << 1) | (1 << 2); // ENABLE, TICKINT, CLKSOURCE

    // Jump to the first task
    rtos_start_first_task(current_tcb->sp);
}
#else
void rtos_start(uint32_t cpu_freq_hz, uint32_t tick_freq_hz) {
    (void)cpu_freq_hz;
    (void)tick_freq_hz;
    uint32_t highest_prio = (uint32_t)__builtin_ctz(g_ready_bitmap);
    for (uint32_t i = 0; i < g_task_count; i++) {
        if (g_tasks[i].current_priority == highest_prio) {
            current_tcb = &g_tasks[i];
            g_tasks[i].ctx_switches = 1;
            g_total_ctx_switches = 1;
            rtos_trace_record(TRACE_TASK_SWITCH, 0, (uint16_t)g_tasks[i].id, highest_prio);
            break;
        }
    }
}
#endif
