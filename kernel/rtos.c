#include "rtos.h"

// Hardware registers for ARM Cortex-M4 vs RISC-V RV32IMAC (omitted in host test mode)
#if defined(RTOS_HOST_TEST)
// Host simulation stubs
#define SCB_CFSR          (0U)
#define SCB_HFSR          (0U)
#define SCB_MMFAR         (0U)
#define SCB_BFAR          (0U)

uint32_t rtos_irq_save(void) { return 0; }
void rtos_irq_restore(uint32_t flags) { (void)flags; }
static inline void rtos_trigger_pendsv(void) {
    // In host simulation, immediately complete the context switch
    extern volatile tcb_t *current_tcb;
    extern volatile tcb_t *next_tcb;
    if (next_tcb != NULL) {
        current_tcb = next_tcb;
    }
}
#elif defined(RTOS_ARCH_RISCV)
#define CLINT_MSIP_REG    (*(volatile uint32_t *)0x20001800UL)
#define SCB_CFSR          ({ uint32_t _v; __asm volatile ("csrr %0, mcause" : "=r"(_v)); _v; })
#define SCB_HFSR          ({ uint32_t _v; __asm volatile ("csrr %0, mstatus" : "=r"(_v)); _v; })
#define SCB_MMFAR         ({ uint32_t _v; __asm volatile ("csrr %0, mtval" : "=r"(_v)); _v; })
#define SCB_BFAR          ({ uint32_t _v; __asm volatile ("csrr %0, mepc" : "=r"(_v)); _v; })

uint32_t rtos_irq_save(void) {
    uint32_t flags;
    __asm volatile ("csrrci %0, mstatus, 0x8" : "=r"(flags) : : "memory");
    return flags;
}

void rtos_irq_restore(uint32_t flags) {
    if (flags & 0x8UL) {
        __asm volatile ("csrsi mstatus, 0x8" : : : "memory");
    }
}

static inline void rtos_trigger_pendsv(void) {
    CLINT_MSIP_REG = 1UL;
}
#else
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

uint32_t rtos_irq_save(void) {
    uint32_t primask;
    __asm volatile ("mrs %0, primask\ncpsid i" : "=r"(primask) : : "memory");
    return primask;
}

void rtos_irq_restore(uint32_t flags) {
    __asm volatile ("msr primask, %0" : : "r"(flags) : "memory");
}

static inline void rtos_trigger_pendsv(void) {
    SCB_ICSR = PENDSVSET_BIT;
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
static rtos_timer_t         *g_timer_list = NULL;

static void rtos_update_ready_bit(uint32_t prio) {
    if (prio >= RTOS_MAX_PRIORITIES) return;
    for (uint32_t i = 0; i < RTOS_MAX_TASKS; i++) {
        if (g_tasks[i].state == TASK_STATE_READY && g_tasks[i].current_priority == prio) {
            g_ready_bitmap |= (1UL << prio);
            return;
        }
    }
    g_ready_bitmap &= ~(1UL << prio);
}

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
    uint32_t flags = rtos_irq_save();
    if (current_tcb) {
        ((tcb_t *)current_tcb)->state = TASK_STATE_SUSPENDED;
        rtos_update_ready_bit(current_tcb->current_priority);
    }
    rtos_irq_restore(flags);
    rtos_schedule();
    while (1) {}
}

void rtos_init(void) {
    g_task_count = 0;
    g_ready_bitmap = 0;
    g_system_ticks = 0;
    g_timer_list = NULL;
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
        g_tasks[i].state = TASK_STATE_UNUSED;
        g_tasks[i].id = 0;
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

tcb_t *rtos_task_create_ex(rtos_task_func_t func, void *arg, uint32_t priority,
                           uint32_t *stack_buf, uint32_t stack_size_words, const char *name) {
    if (!func || !stack_buf || stack_size_words < 32 || priority >= RTOS_MAX_PRIORITIES) {
        return NULL;
    }

    uint32_t flags = rtos_irq_save();
    uint32_t slot = RTOS_MAX_TASKS;
    for (uint32_t i = 0; i < RTOS_MAX_TASKS; i++) {
        if (g_tasks[i].state == TASK_STATE_UNUSED) {
            slot = i;
            break;
        }
    }
    if (slot >= RTOS_MAX_TASKS) {
        rtos_irq_restore(flags);
        return NULL;
    }

    // Fill entire stack with canary pattern (0xDEADBEEF) for high-watermark & overflow detection
    for (uint32_t i = 0; i < stack_size_words; i++) {
        stack_buf[i] = RTOS_STACK_CANARY;
    }

    tcb_t *t = &g_tasks[slot];
    if (slot + 1 > g_task_count) {
        g_task_count = slot + 1;
    }
    t->id = slot + 1;
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

#if defined(RTOS_ARCH_RISCV) && !defined(RTOS_HOST_TEST)
    // Align to 16 bytes per RISC-V ABI and build 32-word (128-byte) initial trap frame
    uint32_t *sp = (uint32_t *)(((uintptr_t)&stack_buf[stack_size_words - 1]) & ~0xFUL);
    sp -= 32;
    for (int i = 0; i < 32; i++) {
        sp[i] = 0x00000000;
    }
    sp[0] = (uint32_t)(uintptr_t)func;                // mepc: Task entry point
    sp[1] = 0x00001880UL;                             // mstatus: MPP=3 (M-mode), MPIE=1
    sp[2] = (uint32_t)(uintptr_t)rtos_task_exit_hook; // x1 (ra): Clean exit hook
    sp[9] = (uint32_t)(uintptr_t)arg;                 // x10 (a0): void *arg parameter
    t->sp = sp;
#else
    // Point to top of stack, aligned to 8 bytes
    uint32_t *sp = &stack_buf[stack_size_words - 1];
    sp = (uint32_t *)((uintptr_t)sp & ~0x7);

    // Build initial Cortex-M4/M7 stack frame
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
#endif

    // Mark priority as ready in bitmap
    g_ready_bitmap |= (1UL << priority);

    rtos_trace_record(TRACE_TASK_CREATE, (uint8_t)t->id, (uint16_t)priority, stack_size_words);
    rtos_irq_restore(flags);

    return t;
}

bool rtos_task_create(rtos_task_func_t func, void *arg, uint32_t priority,
                      uint32_t *stack_buf, uint32_t stack_size_words, const char *name) {
    return rtos_task_create_ex(func, arg, priority, stack_buf, stack_size_words, name) != NULL;
}

void rtos_task_delete(tcb_t *task) {
    uint32_t flags = rtos_irq_save();
    tcb_t *t = task ? task : (tcb_t *)current_tcb;
    if (!t || t == &g_tasks[0] || t->state == TASK_STATE_UNUSED) {
        rtos_irq_restore(flags);
        return;
    }
    uint32_t prio = t->current_priority;
    bool deleting_self = (t == current_tcb);
    t->state = TASK_STATE_UNUSED;
    t->sp = NULL;
    t->stack_base = NULL;
    t->stack_size = 0;
    t->next_blocked = NULL;
    rtos_update_ready_bit(prio);
    rtos_irq_restore(flags);

    if (deleting_self) {
        rtos_schedule();
#ifndef RTOS_HOST_TEST
        while (1) {}
#endif
    }
}

tcb_t *rtos_get_current_task(void) {
    return (tcb_t *)current_tcb;
}

// O(1) Priority Scheduler with Round-Robin across Equal-Priority Ready Tasks
void rtos_schedule(void) {
    uint32_t flags = rtos_irq_save();
    if (g_ready_bitmap == 0) {
        rtos_irq_restore(flags);
        return;
    }

    // Count Trailing Zeros: finds the lowest bit set (which is highest priority, 0 = top)
    uint32_t highest_prio = (uint32_t)__builtin_ctz(g_ready_bitmap);

    // Round-robin scan starting after current_tcb so equal-priority tasks share CPU fairly
    uint32_t start_idx = 0;
    if (current_tcb && current_tcb >= &g_tasks[0] && current_tcb < &g_tasks[RTOS_MAX_TASKS]) {
        start_idx = (uint32_t)((current_tcb - &g_tasks[0]) + 1) % RTOS_MAX_TASKS;
    }

    tcb_t *selected = NULL;
    for (uint32_t n = 0; n < RTOS_MAX_TASKS; n++) {
        uint32_t i = (start_idx + n) % RTOS_MAX_TASKS;
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
    rtos_irq_restore(flags);
}

void SysTick_Handler(void) {
    g_system_ticks++;

    // Sample CPU runtime for currently active task & check stack canary
    if (current_tcb && current_tcb->state != TASK_STATE_UNUSED) {
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

    // Process sleeping/blocked tasks (skip RTOS_WAIT_FOREVER)
    for (uint32_t i = 0; i < RTOS_MAX_TASKS; i++) {
        if (g_tasks[i].state == TASK_STATE_BLOCKED &&
            g_tasks[i].sleep_ticks > 0 &&
            g_tasks[i].sleep_ticks != RTOS_WAIT_FOREVER) {
            if (--g_tasks[i].sleep_ticks == 0) {
                g_tasks[i].state = TASK_STATE_READY;
                g_ready_bitmap |= (1UL << g_tasks[i].current_priority);
                rtos_trace_record(TRACE_TASK_WAKE, (uint8_t)g_tasks[i].id,
                                  (uint16_t)g_tasks[i].current_priority, 0);
            }
        }
    }

    // Service expired software timers
    rtos_timer_t *tmr = g_timer_list;
    while (tmr != NULL) {
        rtos_timer_t *next = tmr->next;
        if (tmr->active && (int32_t)(g_system_ticks - tmr->expire_tick) >= 0) {
            if (tmr->periodic && tmr->period_ticks > 0) {
                tmr->expire_tick = g_system_ticks + tmr->period_ticks;
            } else {
                tmr->active = false;
            }
            if (tmr->callback) {
                tmr->callback(tmr->arg);
            }
        }
        tmr = next;
    }

    rtos_schedule();
}

void rtos_delay(uint32_t ticks) {
    if (ticks == 0 || !current_tcb) return;

    uint32_t flags = rtos_irq_save();
    tcb_t *t = (tcb_t *)current_tcb;
    t->sleep_ticks = ticks;
    t->state = TASK_STATE_BLOCKED;

    rtos_trace_record(TRACE_TASK_DELAY, (uint8_t)t->id, (uint16_t)t->current_priority, ticks);
    rtos_update_ready_bit(t->current_priority);
    rtos_irq_restore(flags);

    rtos_schedule();

#ifndef RTOS_HOST_TEST
    while (t->state == TASK_STATE_BLOCKED) {
        // Spin until context switch occurs
    }
#endif
}

void rtos_yield(void) {
    rtos_schedule();
}

uint32_t rtos_get_tick(void) {
    return g_system_ticks;
}

// -------------------------------------------------------------------------
// Software Timers
// -------------------------------------------------------------------------

void rtos_timer_init(rtos_timer_t *timer, rtos_timer_cb_t cb, void *arg) {
    if (!timer) return;
    timer->callback = cb;
    timer->arg = arg;
    timer->period_ticks = 0;
    timer->expire_tick = 0;
    timer->periodic = false;
    timer->active = false;
    timer->next = NULL;
}

void rtos_timer_start(rtos_timer_t *timer, uint32_t delay_ticks, bool periodic) {
    if (!timer || !timer->callback) return;
    if (delay_ticks == 0) delay_ticks = 1;

    uint32_t flags = rtos_irq_save();
    timer->period_ticks = delay_ticks;
    timer->expire_tick = g_system_ticks + delay_ticks;
    timer->periodic = periodic;
    timer->active = true;

    // Insert into list if not already linked
    bool linked = false;
    for (rtos_timer_t *p = g_timer_list; p != NULL; p = p->next) {
        if (p == timer) {
            linked = true;
            break;
        }
    }
    if (!linked) {
        timer->next = g_timer_list;
        g_timer_list = timer;
    }
    rtos_irq_restore(flags);
}

void rtos_timer_stop(rtos_timer_t *timer) {
    if (!timer) return;
    uint32_t flags = rtos_irq_save();
    timer->active = false;
    rtos_irq_restore(flags);
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

    uint32_t flags = rtos_irq_save();
    if (sem->count > 0) {
        sem->count--;
        rtos_trace_record(TRACE_SEM_WAIT, (uint8_t)current_tcb->id, 1, (uint32_t)sem->count);
        rtos_irq_restore(flags);
        return true;
    }

    // Non-blocking poll (timeout_ticks == 0): fail immediately without blocking
    if (timeout_ticks == 0) {
        rtos_irq_restore(flags);
        return false;
    }

    // Count is 0 -> block current task
    tcb_t *t = (tcb_t *)current_tcb;
    t->state = TASK_STATE_BLOCKED;
    t->sleep_ticks = timeout_ticks;
    rtos_update_ready_bit(t->current_priority);

    rtos_trace_record(TRACE_SEM_WAIT, (uint8_t)t->id, 0, timeout_ticks);

    // Append to wait list
    t->next_blocked = sem->wait_list;
    sem->wait_list = t;
    rtos_irq_restore(flags);

    rtos_schedule();

#ifndef RTOS_HOST_TEST
    while (t->state == TASK_STATE_BLOCKED) {}
#endif

    // Check if we timed out (still present in sem->wait_list) vs woken by rtos_sem_signal()
    flags = rtos_irq_save();
    tcb_t **pp = &sem->wait_list;
    while (*pp != NULL) {
        if (*pp == t) {
            *pp = t->next_blocked;
            t->next_blocked = NULL;
            rtos_irq_restore(flags);
            return false;
        }
        pp = &((*pp)->next_blocked);
    }
    rtos_irq_restore(flags);
    return true;
}

void rtos_sem_signal_from_isr(rtos_sem_t *sem, bool *higher_prio_woken) {
    if (!sem) return;
    if (higher_prio_woken) *higher_prio_woken = false;

    uint32_t flags = rtos_irq_save();
    if (sem->wait_list != NULL) {
        tcb_t *woken = sem->wait_list;
        sem->wait_list = woken->next_blocked;
        woken->next_blocked = NULL;
        woken->state = TASK_STATE_READY;
        woken->sleep_ticks = 0;
        g_ready_bitmap |= (1UL << woken->current_priority);
        rtos_trace_record(TRACE_SEM_SIGNAL, (uint8_t)woken->id, 1, 0);
        if (current_tcb && woken->current_priority < current_tcb->current_priority) {
            if (higher_prio_woken) *higher_prio_woken = true;
            next_tcb = woken;
        }
        rtos_irq_restore(flags);
        return;
    }

    if (sem->count < sem->max_count) {
        sem->count++;
    }
    rtos_trace_record(TRACE_SEM_SIGNAL, current_tcb ? (uint8_t)current_tcb->id : 0, 0, (uint32_t)sem->count);
    rtos_irq_restore(flags);
}

void rtos_sem_signal(rtos_sem_t *sem) {
    if (!sem) return;

    uint32_t flags = rtos_irq_save();
    if (sem->wait_list != NULL) {
        // Wake the first blocked task
        tcb_t *woken = sem->wait_list;
        sem->wait_list = woken->next_blocked;
        woken->next_blocked = NULL;
        woken->state = TASK_STATE_READY;
        woken->sleep_ticks = 0;
        g_ready_bitmap |= (1UL << woken->current_priority);
        rtos_trace_record(TRACE_SEM_SIGNAL, (uint8_t)woken->id, 1, 0);
        rtos_irq_restore(flags);
        rtos_schedule();
        return;
    }

    if (sem->count < sem->max_count) {
        sem->count++;
    }
    rtos_trace_record(TRACE_SEM_SIGNAL, current_tcb ? (uint8_t)current_tcb->id : 0, 0, (uint32_t)sem->count);
    rtos_irq_restore(flags);
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

    uint32_t flags = rtos_irq_save();
    tcb_t *curr = (tcb_t *)current_tcb;

    // Recursive lock
    if (mutex->owner == curr) {
        mutex->lock_count++;
        rtos_irq_restore(flags);
        return true;
    }

    if (mutex->owner == NULL) {
        mutex->owner = curr;
        mutex->lock_count = 1;
        mutex->original_priority = curr->current_priority;
        rtos_trace_record(TRACE_MUTEX_LOCK, (uint8_t)curr->id, 1, curr->current_priority);
        rtos_irq_restore(flags);
        return true;
    }

    // Non-blocking try-lock (timeout_ticks == 0)
    if (timeout_ticks == 0) {
        rtos_irq_restore(flags);
        return false;
    }

    // Priority Inheritance: If current task priority is higher (lower number), boost owner
    if (curr->current_priority < mutex->owner->current_priority) {
        uint32_t old_prio = mutex->owner->current_priority;
        mutex->owner->current_priority = curr->current_priority;
        rtos_update_ready_bit(old_prio);
        if (mutex->owner->state == TASK_STATE_READY) {
            g_ready_bitmap |= (1UL << mutex->owner->current_priority);
        }
        rtos_trace_record(TRACE_MUTEX_PI_BOOST, (uint8_t)mutex->owner->id,
                          (uint16_t)old_prio, curr->current_priority);
    }

    curr->state = TASK_STATE_BLOCKED;
    curr->sleep_ticks = timeout_ticks;
    rtos_update_ready_bit(curr->current_priority);

    curr->next_blocked = mutex->wait_list;
    mutex->wait_list = curr;
    rtos_trace_record(TRACE_MUTEX_LOCK, (uint8_t)curr->id, 0, timeout_ticks);
    rtos_irq_restore(flags);

    rtos_schedule();

#ifndef RTOS_HOST_TEST
    while (curr->state == TASK_STATE_BLOCKED) {}
#endif

    // Verify whether we acquired ownership or timed out
    flags = rtos_irq_save();
    if (mutex->owner == curr) {
        rtos_irq_restore(flags);
        return true;
    }
    tcb_t **pp = &mutex->wait_list;
    while (*pp != NULL) {
        if (*pp == curr) {
            *pp = curr->next_blocked;
            curr->next_blocked = NULL;
            rtos_irq_restore(flags);
            return false;
        }
        pp = &((*pp)->next_blocked);
    }
    rtos_irq_restore(flags);
    return false;
}

void rtos_mutex_unlock(rtos_mutex_t *mutex) {
    if (!mutex || !current_tcb || mutex->owner != current_tcb) return;

    uint32_t flags = rtos_irq_save();
    mutex->lock_count--;
    if (mutex->lock_count > 0) {
        rtos_irq_restore(flags);
        return;
    }

    // Restore owner's original priority
    tcb_t *prev_owner = mutex->owner;
    if (prev_owner->current_priority != mutex->original_priority) {
        uint32_t boosted_prio = prev_owner->current_priority;
        prev_owner->current_priority = mutex->original_priority;
        rtos_update_ready_bit(boosted_prio);
        if (prev_owner->state == TASK_STATE_READY) {
            g_ready_bitmap |= (1UL << prev_owner->current_priority);
        }
    }
    mutex->owner = NULL;
    rtos_trace_record(TRACE_MUTEX_UNLOCK, (uint8_t)prev_owner->id,
                      (uint16_t)prev_owner->current_priority, 0);

    // Transfer ownership to waiting task
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

    rtos_irq_restore(flags);
    rtos_schedule();
}

void rtos_queue_init(rtos_queue_t *q, uint32_t *storage_buf, uint32_t capacity) {
    if (!q || !storage_buf || capacity == 0) return;
    q->buffer = storage_buf;
    q->capacity = capacity;
    q->head = 0;
    q->tail = 0;
    q->count = 0;
    rtos_sem_init(&q->items_avail, 0, (int32_t)capacity);
    rtos_sem_init(&q->slots_free, (int32_t)capacity, (int32_t)capacity);
}

bool rtos_queue_send(rtos_queue_t *q, uint32_t msg, uint32_t timeout_ticks) {
    if (!q || !q->buffer) return false;
    if (!rtos_sem_wait(&q->slots_free, timeout_ticks)) {
        return false;
    }

    uint32_t flags = rtos_irq_save();
    q->buffer[q->head] = msg;
    q->head = (q->head + 1U) % q->capacity;
    q->count++;
    rtos_trace_record(TRACE_QUEUE_SEND, current_tcb ? (uint8_t)current_tcb->id : 0,
                      (uint16_t)q->count, msg);
    rtos_irq_restore(flags);

    rtos_sem_signal(&q->items_avail);
    return true;
}

bool rtos_queue_recv(rtos_queue_t *q, uint32_t *out_msg, uint32_t timeout_ticks) {
    if (!q || !q->buffer || !out_msg) return false;
    if (!rtos_sem_wait(&q->items_avail, timeout_ticks)) {
        return false;
    }

    uint32_t flags = rtos_irq_save();
    *out_msg = q->buffer[q->tail];
    q->tail = (q->tail + 1U) % q->capacity;
    q->count--;
    rtos_trace_record(TRACE_QUEUE_RECV, current_tcb ? (uint8_t)current_tcb->id : 0,
                      (uint16_t)q->count, *out_msg);
    rtos_irq_restore(flags);

    rtos_sem_signal(&q->slots_free);
    return true;
}

uint32_t rtos_queue_count(const rtos_queue_t *q) {
    return q ? q->count : 0;
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
    (void)rtos_irq_save();
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
#ifdef RTOS_ARCH_RISCV
extern void bsp_start_mtimer(uint32_t cpu_freq_hz, uint32_t tick_freq_hz);
#endif

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

#ifdef RTOS_ARCH_RISCV
    bsp_start_mtimer(cpu_freq_hz, tick_freq_hz);
    // Enable CLINT Machine Software Interrupt (MSIE = bit 3) & Machine Timer Interrupt (MTIE = bit 7)
    uint32_t mie_mask = (1UL << 3) | (1UL << 7);
    __asm volatile ("csrs mie, %0" : : "r"(mie_mask));
#else
    // Set PendSV priority to lowest (0xFF)
    SCB_SHPR3 |= (0xFFUL << 16);

    // Setup SysTick
    SYST_RVR = (cpu_freq_hz / tick_freq_hz) - 1;
    SYST_CVR = 0;
    SYST_CSR = (1 << 0) | (1 << 1) | (1 << 2); // ENABLE, TICKINT, CLKSOURCE
#endif

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
