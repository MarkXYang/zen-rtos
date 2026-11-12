#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "rtos.h"

// Access internal kernel state for unit verification
extern volatile tcb_t *current_tcb;
extern volatile tcb_t *next_tcb;

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

    assert(current_tcb->state == TASK_STATE_READY);

    // Task requests 5 ticks delay
    rtos_delay(5);
    assert(current_tcb->state == TASK_STATE_BLOCKED);
    assert(current_tcb->sleep_ticks == 5);

    // Simulate 4 SysTicks -> should still be blocked
    for (int i = 0; i < 4; i++) {
        SysTick_Handler();
        assert(current_tcb->state == TASK_STATE_BLOCKED);
    }

    // 5th tick -> task must wake up and be READY
    SysTick_Handler();
    assert(current_tcb->state == TASK_STATE_READY);
    printf("         Task unblocked precisely on 5th SysTick. [PASS]\n");
}

// -------------------------------------------------------------------------
// Test 4: Mutex Priority Inheritance
// -------------------------------------------------------------------------
static void test_mutex_priority_inheritance(void) {
    printf("[TEST 4] Verifying mutex priority inheritance...\n");
    rtos_init();

    rtos_mutex_t mtx;
    rtos_mutex_init(&mtx);

    tcb_t low_task;
    memset(&low_task, 0, sizeof(low_task));
    low_task.id = 1;
    low_task.current_priority = 20;
    low_task.base_priority = 20;
    low_task.state = TASK_STATE_READY;
    low_task.name = "LowPrio";

    // Set LowPrio as current executing task
    current_tcb = &low_task;

    bool locked = rtos_mutex_lock(&mtx, 0);
    assert(locked == true);
    assert(mtx.owner == &low_task);

    // High priority task (priority 2) arrives and tries to acquire mutex
    tcb_t high_task;
    memset(&high_task, 0, sizeof(high_task));
    high_task.id = 2;
    high_task.current_priority = 2;
    high_task.base_priority = 2;
    high_task.state = TASK_STATE_READY;
    high_task.name = "HighPrio";

    current_tcb = &high_task;
    rtos_mutex_lock(&mtx, 10);

    // LowPrio task must now have its priority boosted to 2!
    assert(low_task.current_priority == 2);
    printf("         LowPrio task boosted to Priority 2 via Inheritance. [PASS]\n");

    // Unlock as LowPrio -> priority must revert back to 20
    current_tcb = &low_task;
    rtos_mutex_unlock(&mtx);
    assert(low_task.current_priority == 20);
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

int main(void) {
    printf("====================================================\n");
    printf("      ZenRTOS Host Verification Test Suite         \n");
    printf("====================================================\n");

    test_stack_frame_integrity();
    test_priority_scheduling();
    test_sleep_and_systick_wakeup();
    test_mutex_priority_inheritance();
    test_semaphore_wait_signal();

    printf("====================================================\n");
    printf("  ALL 5 UNIT TESTS PASSED SUCCESSFULLY! (100%%)      \n");
    printf("====================================================\n");
    return 0;
}
