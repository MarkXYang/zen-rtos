#include "rtos.h"

// Hardware registers for ARM Cortex-M (omitted in host test mode)
#ifndef RTOS_HOST_TEST
#define SCB_ICSR          (*(volatile uint32_t *)0xE000ED04)
#define SCB_SHPR3         (*(volatile uint32_t *)0xE000ED20)
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
static inline void rtos_enter_critical(void) {}
static inline void rtos_exit_critical(void) {}
static inline void rtos_trigger_pendsv(void) {}
#endif

// Global kernel state
volatile tcb_t       *current_tcb = NULL;
volatile tcb_t       *next_tcb = NULL;

static tcb_t          g_tasks[RTOS_MAX_TASKS];
static uint32_t       g_task_count = 0;
static uint32_t       g_ready_bitmap = 0;       // Bit N set means priority N has ready task
static volatile uint32_t g_system_ticks = 0;

// Idle task stack & TCB
#define IDLE_STACK_SIZE 128
static uint32_t       g_idle_stack[IDLE_STACK_SIZE];

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
    current_tcb = NULL;
    next_tcb = NULL;

    for (uint32_t i = 0; i < RTOS_MAX_TASKS; i++) {
        g_tasks[i].state = TASK_STATE_SUSPENDED;
        g_tasks[i].sp = NULL;
        g_tasks[i].next_blocked = NULL;
    }

    // Create Idle Task at lowest priority (31)
    rtos_task_create(rtos_idle_task, NULL, 31, g_idle_stack, IDLE_STACK_SIZE, "Idle");
}

bool rtos_task_create(rtos_task_func_t func, void *arg, uint32_t priority,
                      uint32_t *stack_buf, uint32_t stack_size_words, const char *name) {
    if (g_task_count >= RTOS_MAX_TASKS || !func || !stack_buf || priority >= RTOS_MAX_PRIORITIES) {
        return false;
    }

    tcb_t *t = &g_tasks[g_task_count++];
    t->id = g_task_count;
    t->base_priority = priority;
    t->current_priority = priority;
    t->state = TASK_STATE_READY;
    t->sleep_ticks = 0;
    t->name = name ? name : "Task";
    t->next_blocked = NULL;

    // Point to top of stack, aligned to 8 bytes
    uint32_t *sp = &stack_buf[stack_size_words - 1];
    sp = (uint32_t *)((uintptr_t)sp & ~0x7);

    // Build initial Cortex-M4 stack frame
    // 1. Hardware auto-saved registers
    *(--sp) = 0x01000000;                       // xPSR: Default Thumb mode bit (Bit 24)
    *(--sp) = (uint32_t)(uintptr_t)func;       // PC: Task entry point
    *(--sp) = (uint32_t)(uintptr_t)rtos_task_exit_hook; // LR: Clean exit if task returns
    *(--sp) = 0x00000000;                       // R12
    *(--sp) = 0x00000000;                       // R3
    *(--sp) = 0x00000000;                       // R2
    *(--sp) = 0x00000000;                       // R1
    *(--sp) = (uint32_t)(uintptr_t)arg;        // R0: void *arg parameter

    // 2. Software saved registers (R4 - R11)
    for (int i = 0; i < 8; i++) {
        *(--sp) = 0x00000000;
    }

    t->sp = sp;

    // Mark priority as ready in bitmap
    g_ready_bitmap |= (1UL << priority);

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
        next_tcb = selected;
        rtos_trigger_pendsv();
    }
}

void SysTick_Handler(void) {
    g_system_ticks++;

    // Process sleeping/blocked tasks
    for (uint32_t i = 0; i < g_task_count; i++) {
        if (g_tasks[i].state == TASK_STATE_BLOCKED && g_tasks[i].sleep_ticks > 0) {
            if (--g_tasks[i].sleep_ticks == 0) {
                g_tasks[i].state = TASK_STATE_READY;
                g_ready_bitmap |= (1UL << g_tasks[i].current_priority);
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
        rtos_exit_critical();
        return true;
    }

    // Count is 0 -> block current task
    tcb_t *t = (tcb_t *)current_tcb;
    t->state = TASK_STATE_BLOCKED;
    t->sleep_ticks = timeout_ticks;
    g_ready_bitmap &= ~(1UL << t->current_priority);

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
        rtos_exit_critical();
        rtos_schedule();
        return;
    }

    if (sem->count < sem->max_count) {
        sem->count++;
    }
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
    }

    curr->state = TASK_STATE_BLOCKED;
    curr->sleep_ticks = timeout_ticks;
    g_ready_bitmap &= ~(1UL << curr->current_priority);

    curr->next_blocked = mutex->wait_list;
    mutex->wait_list = curr;
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

#ifndef RTOS_HOST_TEST
extern void rtos_start_first_task(uint32_t *sp);

void rtos_start(uint32_t cpu_freq_hz, uint32_t tick_freq_hz) {
    // Select the highest priority ready task
    uint32_t highest_prio = (uint32_t)__builtin_ctz(g_ready_bitmap);
    for (uint32_t i = 0; i < g_task_count; i++) {
        if (g_tasks[i].current_priority == highest_prio) {
            current_tcb = &g_tasks[i];
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
            break;
        }
    }
}
#endif
