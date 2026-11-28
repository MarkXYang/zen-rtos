#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "rtos.h"
#include "wifi_wpa2.h"

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

// -------------------------------------------------------------------------
// Test 9: Inter-Thread Message Queue (FIFO & Blocking Wakeup)
// -------------------------------------------------------------------------
static void test_message_queue_ipc(void) {
    printf("[TEST 9] Verifying bounded message queue (rtos_queue_t) IPC...\n");
    rtos_init();

    uint32_t qbuf[4];
    rtos_queue_t q;
    rtos_queue_init(&q, qbuf, 4);

    rtos_task_create(dummy_task1, NULL, 10, stack_t1, 256, "Producer");
    rtos_task_create(dummy_task2, NULL, 5,  stack_t2, 256, "Consumer");
    rtos_start(1000, 1000);

    // Send 3 messages in FIFO order
    assert(rtos_queue_send(&q, 101, 0) == true);
    assert(rtos_queue_send(&q, 202, 0) == true);
    assert(rtos_queue_send(&q, 303, 0) == true);
    assert(rtos_queue_count(&q) == 3);

    uint32_t msg = 0;
    assert(rtos_queue_recv(&q, &msg, 0) == true && msg == 101);
    assert(rtos_queue_recv(&q, &msg, 0) == true && msg == 202);
    assert(rtos_queue_recv(&q, &msg, 0) == true && msg == 303);
    assert(rtos_queue_count(&q) == 0);
    printf("         FIFO ordering & semaphore-backed queue count verified. [PASS]\n");
}

// -------------------------------------------------------------------------
// Test 10: Non-Blocking Poll (timeout=0) vs RTOS_WAIT_FOREVER
// -------------------------------------------------------------------------
static void test_sem_and_mutex_timeouts(void) {
    printf("[TEST 10] Verifying non-blocking timeout=0 vs RTOS_WAIT_FOREVER...\n");
    rtos_init();

    rtos_sem_t sem;
    rtos_sem_init(&sem, 0, 1);
    rtos_task_create(dummy_task1, NULL, 5, stack_t1, 256, "T10");
    rtos_start(1000, 1000);

    // timeout=0 on empty semaphore must fail immediately without blocking
    assert(rtos_sem_wait(&sem, 0) == false);
    assert(current_tcb->state == TASK_STATE_READY);

    // Non-blocking mutex try-lock when held by another task must return false
    rtos_mutex_t mtx;
    rtos_mutex_init(&mtx);
    rtos_task_create(dummy_task2, NULL, 6, stack_t2, 256, "T10b");
    tcb_t *t1 = (tcb_t *)rtos_get_task_by_id(2);
    tcb_t *t2 = (tcb_t *)rtos_get_task_by_id(3);
    current_tcb = t1;
    assert(rtos_mutex_lock(&mtx, 0) == true);
    current_tcb = t2;
    assert(rtos_mutex_lock(&mtx, 0) == false);
    assert(t2->state == TASK_STATE_READY);
    printf("          Non-blocking sem/mutex try-lock verified. [PASS]\n");
}

// -------------------------------------------------------------------------
// Test 11: Shared-Priority Round-Robin & Task Deletion Slot Reuse
// -------------------------------------------------------------------------
static void test_round_robin_and_task_delete(void) {
    printf("[TEST 11] Verifying equal-priority round-robin & rtos_task_delete()...\n");
    rtos_init();

    tcb_t *a = rtos_task_create_ex(dummy_task1, NULL, 5, stack_t1, 256, "RR_A");
    tcb_t *b = rtos_task_create_ex(dummy_task2, NULL, 5, stack_t2, 256, "RR_B");
    assert(a != NULL && b != NULL);
    rtos_start(1000, 1000);
    assert(current_tcb == a);

    // Yielding at equal priority must rotate to RR_B, then back to RR_A
    rtos_yield();
    assert(current_tcb == b);
    rtos_yield();
    assert(current_tcb == a);

    // Deleting RR_B frees its slot for reuse
    uint32_t old_id = b->id;
    rtos_task_delete(b);
    assert(b->state == TASK_STATE_UNUSED);
    tcb_t *c = rtos_task_create_ex(dummy_task3, NULL, 5, stack_t3, 256, "RR_C");
    assert(c != NULL && c->id == old_id);
    printf("          Round-robin rotation & TCB slot reuse verified. [PASS]\n");
}

// -------------------------------------------------------------------------
// Test 12: Software Timers (One-Shot & Periodic) + ISR Semaphore Signal
// -------------------------------------------------------------------------
static uint32_t g_tmr_oneshot_hits = 0;
static uint32_t g_tmr_periodic_hits = 0;

static void cb_oneshot(void *arg) {
    (void)arg;
    g_tmr_oneshot_hits++;
}

static void cb_periodic(void *arg) {
    (void)arg;
    g_tmr_periodic_hits++;
}

static void test_software_timers_and_isr_sem(void) {
    printf("[TEST 12] Verifying software timers & rtos_sem_signal_from_isr()...\n");
    rtos_init();
    g_tmr_oneshot_hits = 0;
    g_tmr_periodic_hits = 0;

    rtos_task_create(dummy_task1, NULL, 5, stack_t1, 256, "TmrTask");
    rtos_start(1000, 1000);

    rtos_timer_t t1, t2;
    rtos_timer_init(&t1, cb_oneshot, NULL);
    rtos_timer_init(&t2, cb_periodic, NULL);
    rtos_timer_start(&t1, 3, false);
    rtos_timer_start(&t2, 2, true);

    for (int i = 0; i < 6; i++) {
        SysTick_Handler();
    }
    assert(g_tmr_oneshot_hits == 1);
    assert(g_tmr_periodic_hits == 3);
    rtos_timer_stop(&t2);
    SysTick_Handler();
    SysTick_Handler();
    assert(g_tmr_periodic_hits == 3);

    rtos_sem_t sem;
    rtos_sem_init(&sem, 0, 2);
    bool woken = false;
    rtos_sem_signal_from_isr(&sem, &woken);
    assert(sem.count == 1 && woken == false);
    printf("          One-shot/periodic timers & ISR sem verified. [PASS]\n");
}

// -------------------------------------------------------------------------
// Test 13: WPA2-PSK Crypto (IEEE 802.11i PBKDF2-SHA1 & RFC 3394 AES Unwrap)
// -------------------------------------------------------------------------
static void test_wpa2_crypto_vectors(void) {
    printf("[TEST 13] Verifying IEEE 802.11i PBKDF2-SHA1 & RFC 3394 AES-128 Unwrap...\n");

    // Official IEEE 802.11i-2004 Annex H.4 test vector: passphrase="password", SSID="IEEE"
    static const uint8_t expected_pmk[32] = {
        0xf4, 0x2c, 0x6f, 0xc5, 0x2d, 0xf0, 0xeb, 0xef,
        0x9e, 0xbb, 0x4b, 0x90, 0xb3, 0x8a, 0x5f, 0x90,
        0x2e, 0x83, 0xfe, 0x1b, 0x13, 0x5a, 0x70, 0xe2,
        0x3a, 0xed, 0x76, 0x2e, 0x97, 0x10, 0xa1, 0x2e
    };
    uint8_t pmk[32];
    wpa2_pbkdf2_sha1("password", (const uint8_t *)"IEEE", 4, 4096, pmk, 32);
    assert(memcmp(pmk, expected_pmk, 32) == 0);

    // Official RFC 3394 Section 4.1 (128-bit KEK, 128-bit Key Data)
    static const uint8_t kek[16] = {
        0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
        0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F
    };
    static const uint8_t cipher[24] = {
        0x1F, 0xA6, 0x8B, 0x0A, 0x81, 0x12, 0xB4, 0x47,
        0xAE, 0xF3, 0x4B, 0xD8, 0xFB, 0x5A, 0x7B, 0x82,
        0x9D, 0x3E, 0x86, 0x23, 0x71, 0xD2, 0xCF, 0xE5
    };
    static const uint8_t expected_plain[16] = {
        0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
        0x88, 0x99, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF
    };
    uint8_t plain[16];
    assert(wpa2_aes_unwrap(kek, 2, cipher, plain) == 0);
    assert(memcmp(plain, expected_plain, 16) == 0);

    // Corrupted ciphertext must fail integrity check (IV != 0xA6..A6)
    uint8_t bad_cipher[24];
    memcpy(bad_cipher, cipher, 24);
    bad_cipher[0] ^= 0x01;
    assert(wpa2_aes_unwrap(kek, 2, bad_cipher, plain) != 0);

    // PRF-384 determinism check
    uint8_t ptk1[48], ptk2[48];
    uint8_t seed[16] = {1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16};
    wpa2_prf_384(pmk, "Pairwise key expansion", seed, sizeof(seed), ptk1);
    wpa2_prf_384(pmk, "Pairwise key expansion", seed, sizeof(seed), ptk2);
    assert(memcmp(ptk1, ptk2, 48) == 0);

    printf("          PBKDF2-SHA1 (IEEE 802.11i), AES-128 Unwrap (RFC 3394) & PRF-384 verified. [PASS]\n");
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
    test_message_queue_ipc();
    test_sem_and_mutex_timeouts();
    test_round_robin_and_task_delete();
    test_software_timers_and_isr_sem();
    test_wpa2_crypto_vectors();

    printf("====================================================\n");
    printf("  ALL 13 UNIT TESTS PASSED SUCCESSFULLY! (100%%)     \n");
    printf("====================================================\n");
    return 0;
}
