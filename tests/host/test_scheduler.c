#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "rtos.h"

// Access internal kernel state for unit verification
extern volatile tcb_t *current_tcb;
extern volatile tcb_t *next_tcb;
extern volatile rtos_fault_info_t g_fault_info;

static uint32_t stack_t1[256];
static uint32_t stack_t2[256];
static uint32_t stack_t3[256];

static int task1_run_count = 0;
static int task2_run_count = 0;

static void dummy_task1(void *arg) {
    (void)arg;
    task1_run_count++;
}

static void dummy_task2(void *arg) {
    (void)arg;
    task2_run_count++;
}

static void dummy_task3(void *arg) {
    (void)arg;
}

// -------------------------------------------------------------------------
// Test 1: Stack Frame Integrity and Alignment
// -------------------------------------------------------------------------
static void test_stack_frame_integrity(void) {
    printf("[TEST 1] Verifying task stack initialization and alignment...\n");
    rtos_init();

    bool ok = rtos_task_create(dummy_task1, (void *)0xCAFE1234, 5, stack_t1, 256, "T1");
    assert(ok == true);

    // Check stack pointer of created task
    uint32_t *sp = stack_t1 + 256 - 1;
    sp = (uint32_t *)((uintptr_t)sp & ~0x7);
    // Stack frame contains 16 words (8 hardware + 8 software)
    sp -= 16;

    // sp[15] is xPSR (offset 15 from bottom of frame)
    assert((sp[15] & (1UL << 24)) != 0); // Thumb bit set
    assert(sp[14] == (uint32_t)(uintptr_t)dummy_task1); // PC set to task entry
    assert(sp[8] == 0xCAFE1234); // R0 set to argument
    printf("         Stack frame integrity verified. [PASS]\n");
}

// -------------------------------------------------------------------------
// Test 2: Priority Resolution (O(1) Bitmask)
// -------------------------------------------------------------------------
static void test_priority_scheduling(void) {
    printf("[TEST 2] Verifying strict priority preemptive scheduler...\n");
    rtos_init();

    // Create tasks at priority 15, 3, and 8
    rtos_task_create(dummy_task1, NULL, 15, stack_t1, 256, "LowPrio");
    rtos_task_create(dummy_task2, NULL, 3,  stack_t2, 256, "HighPrio");
    rtos_task_create(dummy_task3, NULL, 8,  stack_t3, 256, "MidPrio");

    // Start scheduler
    rtos_start(1000, 1000);

    // The selected task must be priority 3
    assert(current_tcb != NULL);
    assert(current_tcb->current_priority == 3);
    assert(strcmp(current_tcb->name, "HighPrio") == 0);
    printf("         Highest priority task (Priority 3) selected. [PASS]\n");
}

// -------------------------------------------------------------------------
// Test 3: Sleep Queue and Wakeup via SysTick
// -------------------------------------------------------------------------
static void test_sleep_and_systick_wakeup(void) {
    printf("[TEST 3] Verifying rtos_delay() and SysTick wakeup...\n");
    rtos_init();

    rtos_task_create(dummy_task1, NULL, 5, stack_t1, 256, "Worker");
    rtos_start(1000, 1000);

    const tcb_t *worker = rtos_get_task_by_id(2); // ID 1 is Idle, ID 2 is Worker
    assert(worker != NULL);
    assert(current_tcb == worker);
    assert(worker->state == TASK_STATE_READY);

    // Task requests 5 ticks delay -> switches to Idle (ID 1) during sleep
    rtos_delay(5);
    assert(worker->state == TASK_STATE_BLOCKED);
    assert(worker->sleep_ticks == 5);
    assert(current_tcb->id == 1); // Idle task running while Worker sleeps

    // Simulate 4 SysTicks -> Worker should still be blocked
    for (int i = 0; i < 4; i++) {
        SysTick_Handler();
        assert(worker->state == TASK_STATE_BLOCKED);
    }

    // 5th tick -> Worker must wake up, become READY, and preempt Idle
    SysTick_Handler();
    assert(worker->state == TASK_STATE_READY);
    assert(current_tcb == worker);
    printf("         Task unblocked and preempted Idle on 5th SysTick. [PASS]\n");
}

// -------------------------------------------------------------------------
// Test 4: Mutex Priority Inheritance
// -------------------------------------------------------------------------
static void test_mutex_priority_inheritance(void) {
    printf("[TEST 4] Verifying mutex priority inheritance...\n");
    rtos_init();

    rtos_mutex_t mtx;
    rtos_mutex_init(&mtx);

    rtos_task_create(dummy_task1, NULL, 20, stack_t1, 256, "LowPrio");
    rtos_task_create(dummy_task2, NULL, 2,  stack_t2, 256, "HighPrio");

    tcb_t *low_task  = (tcb_t *)rtos_get_task_by_id(2);
    tcb_t *high_task = (tcb_t *)rtos_get_task_by_id(3);

    // Set LowPrio as current executing task and lock mutex
    current_tcb = low_task;
    bool locked = rtos_mutex_lock(&mtx, 0);
    assert(locked == true);
    assert(mtx.owner == low_task);

    // High priority task (priority 2) arrives and tries to acquire mutex
    current_tcb = high_task;
    rtos_mutex_lock(&mtx, 10);

    // LowPrio task must now have its priority boosted to 2!
    assert(low_task->current_priority == 2);
    printf("         LowPrio task boosted to Priority 2 via Inheritance. [PASS]\n");

    // Unlock as LowPrio -> priority must revert back to 20
    current_tcb = low_task;
    rtos_mutex_unlock(&mtx);
    assert(low_task->current_priority == 20);
    printf("         LowPrio task restored to base priority 20 upon unlock. [PASS]\n");
}

// -------------------------------------------------------------------------
// Test 5: Semaphore Wait & Signal
// -------------------------------------------------------------------------
static void test_semaphore_wait_signal(void) {
    printf("[TEST 5] Verifying counting and binary semaphore...\n");
    rtos_init();

    rtos_sem_t sem;
    rtos_sem_init(&sem, 1, 1);

    rtos_task_create(dummy_task1, NULL, 5, stack_t1, 256, "SemWorker");
    rtos_start(1000, 1000);

    assert(rtos_sem_wait(&sem, 0) == true);
    assert(sem.count == 0);

    // Signaling should restore token
    rtos_sem_signal(&sem);
    assert(sem.count == 1);
    printf("         Semaphore operations verified. [PASS]\n");
}

// -------------------------------------------------------------------------
// Test 6: Stack Canary High-Watermark & Overflow Detection
// -------------------------------------------------------------------------
static void test_stack_watermark_and_overflow(void) {
    printf("[TEST 6] Verifying stack canary watermark & overflow detector...\n");
    rtos_init();

    rtos_task_create(dummy_task1, NULL, 5, stack_t1, 256, "StackTask");
    // Task ID 1 is Idle, Task ID 2 is StackTask
    uint32_t free_words = rtos_task_stack_free_words(2);
    // 256 total words minus ~17 words for initial aligned Cortex-M4 frame = 238..240 free words
    assert(free_words >= 238 && free_words <= 240);
    assert(rtos_check_stack_overflow() == 0);

    // Simulate stack corruption at bottom canary of StackTask
    stack_t1[0] = 0xBADCAFE0U;
    uint32_t corrupted_id = rtos_check_stack_overflow();
    assert(corrupted_id == 2);
    printf("         Stack watermark (%u/256 words free) & overflow detection verified. [PASS]\n",
           free_words);
}

// -------------------------------------------------------------------------
// Test 7: Runtime Profiling Statistics & CPU Load Calculation
// -------------------------------------------------------------------------
static void test_runtime_stats_and_cpu_load(void) {
    printf("[TEST 7] Verifying runtime task stats and CPU load percentage...\n");
    rtos_init();

    rtos_task_create(dummy_task1, NULL, 5, stack_t1, 256, "ActiveWorker");
    rtos_start(1000, 1000);

    // Run ActiveWorker for 6 ticks
    for (int i = 0; i < 6; i++) {
        SysTick_Handler();
    }

    // Delay ActiveWorker for 4 ticks -> Idle task runs for 4 ticks
    rtos_delay(4);
    for (int i = 0; i < 4; i++) {
        SysTick_Handler();
    }

    rtos_stats_t stats;
    rtos_get_stats(&stats);
    assert(stats.uptime_ticks == 10);
    assert(stats.idle_ticks == 4);
    assert(stats.cpu_load_percent == 60); // 6 active ticks / 10 total ticks = 60%
    assert(stats.total_ctx_switches >= 3);
    printf("         CPU load (60%%), uptime (10 ticks), CtxSw (%u) verified. [PASS]\n",
           stats.total_ctx_switches);
}

// -------------------------------------------------------------------------
// Test 8: Circular Trace Ring Buffer & Fault/Assertion Analyzer
// -------------------------------------------------------------------------
static void test_trace_buffer_and_fault_capture(void) {
    printf("[TEST 8] Verifying circular trace ring buffer & fault capture...\n");
    rtos_init();

    rtos_task_create(dummy_task1, NULL, 5, stack_t1, 256, "TraceTask");
    rtos_start(1000, 1000);

    const rtos_trace_event_t *trace_buf = NULL;
    uint32_t head = 0;
    uint32_t count = rtos_trace_get_events(&trace_buf, &head);
    assert(trace_buf != NULL);
    assert(count >= 3); // Idle create, TraceTask create, initial task switch

    // Simulate a Cortex-M4 exception frame on stack and invoke fault analyzer
    uint32_t fake_frame[8] = {
        0x11111111, // R0
        0x22222222, // R1
        0x33333333, // R2
        0x44444444, // R3
        0x55555555, // R12
        0x08000185, // LR
        0x080002A4, // PC (Faulting instruction address)
        0x01000000  // xPSR
    };
    rtos_fault_capture(fake_frame);
    assert(g_fault_info.active == 1);
    assert(g_fault_info.pc == 0x080002A4);
    assert(g_fault_info.lr == 0x08000185);
    assert(g_fault_info.offending_task_id == 2);

    // Test RTOS_ASSERT capture
    RTOS_ASSERT(1 == 0);
    assert(g_fault_info.assert_line > 0);
    assert(g_fault_info.assert_file != NULL);
    printf("         Trace ring buffer (%u events) & Fault/Assert analyzer verified. [PASS]\n",
           count);
}

int main(void) {
    printf("====================================================\n");
    printf("      ZenRTOS Host Verification Test Suite         \n");
    printf("====================================================\n");

    test_stack_frame_integrity();
    test_priority_scheduling();
    test_sleep_and_systick_wakeup();
    test_mutex_priority_inheritance();
    test_semaphore_wait_signal();
    test_stack_watermark_and_overflow();
    test_runtime_stats_and_cpu_load();
    test_trace_buffer_and_fault_capture();

    printf("====================================================\n");
    printf("  ALL 8 UNIT TESTS PASSED SUCCESSFULLY! (100%%)      \n");
    printf("====================================================\n");
    return 0;
}
