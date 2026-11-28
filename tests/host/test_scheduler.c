#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "rtos.h"
#include "wifi_wpa2.h"
#include "zen_net.h"
#include "zen_net_utils.h"

void bsp_uart_puts(const char *s) { (void)s; }

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

// -------------------------------------------------------------------------
// Test 14 & 15: ZenNet Micro-IPv4 Stack (ARP, DHCPv4, ICMPv4, UDP)
// -------------------------------------------------------------------------
static uint8_t  g_mock_tx_buf[1518];
static uint16_t g_mock_tx_len = 0;
static uint32_t g_mock_tx_cnt = 0;

static int32_t mock_eth_tx(const uint8_t *frame, uint16_t len) {
    if (len > sizeof(g_mock_tx_buf)) return -1;
    memcpy(g_mock_tx_buf, frame, len);
    g_mock_tx_len = len;
    g_mock_tx_cnt++;
    return 0;
}

static void build_mock_ipv4_udp(uint8_t *out, uint16_t *out_len,
                                const uint8_t src_mac[6], const uint8_t dst_mac[6],
                                uint32_t src_ip, uint32_t dst_ip,
                                uint16_t src_port, uint16_t dst_port,
                                const uint8_t *payload, uint16_t plen) {
    uint16_t udp_len = (uint16_t)(8 + plen);
    uint16_t ip_len  = (uint16_t)(20 + udp_len);
    memset(out, 0, 14 + ip_len);
    memcpy(out + 0, dst_mac, 6);
    memcpy(out + 6, src_mac, 6);
    out[12] = 0x08; out[13] = 0x00;

    uint8_t *ip = out + 14;
    ip[0] = 0x45;
    ip[2] = (uint8_t)(ip_len >> 8); ip[3] = (uint8_t)(ip_len & 0xFF);
    ip[8] = 64; ip[9] = 17;
    ip[12] = (uint8_t)(src_ip >> 24); ip[13] = (uint8_t)(src_ip >> 16);
    ip[14] = (uint8_t)(src_ip >> 8);  ip[15] = (uint8_t)(src_ip);
    ip[16] = (uint8_t)(dst_ip >> 24); ip[17] = (uint8_t)(dst_ip >> 16);
    ip[18] = (uint8_t)(dst_ip >> 8);  ip[19] = (uint8_t)(dst_ip);
    uint16_t csum = zen_inet_checksum(ip, 20);
    ip[10] = (uint8_t)(csum >> 8); ip[11] = (uint8_t)(csum & 0xFF);

    uint8_t *udp = ip + 20;
    udp[0] = (uint8_t)(src_port >> 8); udp[1] = (uint8_t)(src_port);
    udp[2] = (uint8_t)(dst_port >> 8); udp[3] = (uint8_t)(dst_port);
    udp[4] = (uint8_t)(udp_len >> 8);  udp[5] = (uint8_t)(udp_len);
    memcpy(udp + 8, payload, plen);
    *out_len = (uint16_t)(14 + ip_len);
}

static void test_zen_net_arp_and_dhcp(void) {
    printf("[TEST 14] Verifying ZenNet IPv4 helpers, ARP reply & DHCPv4 handshake...\n");
    rtos_init();
    rtos_task_create(dummy_task1, NULL, 5, stack_t1, 256, "NetTask");
    rtos_start(1000, 1000);

    // 1. IPv4 parse & format + RFC 1071 checksum
    uint32_t ip = 0;
    assert(zen_ip4_parse("192.168.1.142", &ip) && ip == 0xC0A8018EU);
    assert(!zen_ip4_parse("999.1.2.3", &ip));
    char ip_str[16];
    zen_ip4_format(0xC0A8018EU, ip_str, sizeof(ip_str));
    assert(strcmp(ip_str, "192.168.1.142") == 0);

    // 2. Init ZenNet & trigger link-up -> must emit DHCPDISCOVER
    static const uint8_t sta_mac[6] = { 0x40, 0x4C, 0xCA, 0x53, 0xA2, 0xA4 };
    static const uint8_t gw_mac[6]  = { 0x10, 0x6F, 0x3F, 0x64, 0xDC, 0xC2 };
    g_mock_tx_cnt = 0;
    zen_net_init(sta_mac, (void *)mock_eth_tx);
    zen_net_set_link(true);
    assert(zen_net_get_dhcp_state() == ZEN_DHCP_SELECTING);
    assert(g_mock_tx_cnt == 1);
    assert(g_mock_tx_len >= 14 + 20 + 8 + 240);
    // Extract DHCP XID from transmitted DISCOVER
    uint32_t xid = ((uint32_t)g_mock_tx_buf[46] << 24) | ((uint32_t)g_mock_tx_buf[47] << 16) |
                   ((uint32_t)g_mock_tx_buf[48] << 8)  | (uint32_t)g_mock_tx_buf[49];

    // 3. Inject DHCPOFFER (192.168.1.142 from server 192.168.1.1)
    uint8_t dhcp[300];
    memset(dhcp, 0, sizeof(dhcp));
    dhcp[0] = 2; dhcp[1] = 1; dhcp[2] = 6;
    dhcp[4] = (uint8_t)(xid >> 24); dhcp[5] = (uint8_t)(xid >> 16);
    dhcp[6] = (uint8_t)(xid >> 8);  dhcp[7] = (uint8_t)(xid);
    dhcp[16] = 192; dhcp[17] = 168; dhcp[18] = 1; dhcp[19] = 142; // yiaddr
    memcpy(dhcp + 28, sta_mac, 6);
    dhcp[236] = 99; dhcp[237] = 130; dhcp[238] = 83; dhcp[239] = 99;
    uint8_t opts_offer[] = {
        53, 1, 2,                          // DHCPOFFER
        54, 4, 192, 168, 1, 1,             // Server ID
        1,  4, 255, 255, 255, 0,           // Netmask
        3,  4, 192, 168, 1, 1,             // Gateway
        6,  4, 8, 8, 8, 8,                 // DNS
        51, 4, 0, 0, 0x0E, 0x10,           // Lease 3600s
        255
    };
    memcpy(dhcp + 240, opts_offer, sizeof(opts_offer));
    uint8_t frame[512];
    uint16_t flen = 0;
    build_mock_ipv4_udp(frame, &flen, gw_mac, sta_mac, 0xC0A80101U, 0xFFFFFFFFU, 67, 68, dhcp, sizeof(dhcp));
    zen_net_rx_frame(frame, flen);
    assert(zen_net_get_dhcp_state() == ZEN_DHCP_REQUESTING);
    assert(g_mock_tx_cnt == 2); // Sent DHCPREQUEST

    // 4. Inject DHCPACK -> must bind IP/Mask/GW/DNS and send ARP probe for GW
    dhcp[242] = 5; // DHCPACK
    build_mock_ipv4_udp(frame, &flen, gw_mac, sta_mac, 0xC0A80101U, 0xFFFFFFFFU, 67, 68, dhcp, sizeof(dhcp));
    zen_net_rx_frame(frame, flen);
    assert(zen_net_get_dhcp_state() == ZEN_DHCP_BOUND);
    assert(zen_net_is_up());
    assert(zen_net_get_ip() == 0xC0A8018EU);
    assert(zen_net_get_netmask() == 0xFFFFFF00U);
    assert(zen_net_get_gateway() == 0xC0A80101U);
    assert(zen_net_get_dns() == 0x08080808U);

    // 5. Inject ARP Request for our IP (192.168.1.142) -> must transmit ARP Reply
    uint8_t arp_req[42];
    memset(arp_req, 0, sizeof(arp_req));
    memset(arp_req, 0xFF, 6);
    memcpy(arp_req + 6, gw_mac, 6);
    arp_req[12] = 0x08; arp_req[13] = 0x06;
    arp_req[15] = 1; arp_req[16] = 0x08; arp_req[18] = 6; arp_req[19] = 4; arp_req[21] = 1;
    memcpy(arp_req + 22, gw_mac, 6);
    arp_req[28] = 192; arp_req[29] = 168; arp_req[30] = 1; arp_req[31] = 1;
    arp_req[38] = 192; arp_req[39] = 168; arp_req[40] = 1; arp_req[41] = 142;
    uint32_t prev_tx = g_mock_tx_cnt;
    zen_net_rx_frame(arp_req, sizeof(arp_req));
    assert(g_mock_tx_cnt == prev_tx + 1);
    assert(g_mock_tx_buf[12] == 0x08 && g_mock_tx_buf[13] == 0x06);
    assert(g_mock_tx_buf[21] == 2); // ARP Reply
    assert(memcmp(g_mock_tx_buf + 22, sta_mac, 6) == 0);

    printf("          IPv4 parse/format, DHCPv4 4-step bind & ARP reply verified. [PASS]\n");
}

static void test_zen_net_icmp_and_udp(void) {
    printf("[TEST 15] Verifying ZenNet ICMPv4 echo reply & UDP socket API...\n");
    static const uint8_t sta_mac[6] = { 0x40, 0x4C, 0xCA, 0x53, 0xA2, 0xA4 };
    static const uint8_t gw_mac[6]  = { 0x10, 0x6F, 0x3F, 0x64, 0xDC, 0xC2 };

    // 1. Inbound ICMPv4 Echo Request from 192.168.1.1 -> must send ICMP Echo Reply (type=0)
    uint8_t frame[128];
    memset(frame, 0, sizeof(frame));
    memcpy(frame + 0, sta_mac, 6);
    memcpy(frame + 6, gw_mac, 6);
    frame[12] = 0x08; frame[13] = 0x00;
    uint8_t *ip = frame + 14;
    ip[0] = 0x45; ip[3] = 20 + 12; ip[8] = 64; ip[9] = 1;
    ip[12] = 192; ip[13] = 168; ip[14] = 1; ip[15] = 1;
    ip[16] = 192; ip[17] = 168; ip[18] = 1; ip[19] = 142;
    uint16_t ip_csum = zen_inet_checksum(ip, 20);
    ip[10] = (uint8_t)(ip_csum >> 8); ip[11] = (uint8_t)(ip_csum & 0xFF);
    uint8_t *icmp = ip + 20;
    icmp[0] = 8; icmp[4] = 0x12; icmp[5] = 0x34; icmp[6] = 0x00; icmp[7] = 0x01;
    memcpy(icmp + 8, "PING", 4);
    uint16_t ic_csum = zen_inet_checksum(icmp, 12);
    icmp[2] = (uint8_t)(ic_csum >> 8); icmp[3] = (uint8_t)(ic_csum & 0xFF);

    uint32_t prev_tx = g_mock_tx_cnt;
    zen_net_rx_frame(frame, 14 + 20 + 12);
    assert(g_mock_tx_cnt == prev_tx + 1);
    assert(g_mock_tx_buf[23] == 1);  // IP proto ICMP
    assert(g_mock_tx_buf[34] == 0);  // ICMP Echo Reply
    assert(memcmp(g_mock_tx_buf + 42, "PING", 4) == 0);

    // 2. UDP bind + inbound datagram + recvfrom + sendto
    int32_t slot = zen_udp_bind(9000);
    assert(slot >= 0 && zen_udp_slot_port(slot) == 9000);
    const char *msg = "Hello-ZenNet";
    uint16_t flen = 0;
    build_mock_ipv4_udp(frame, &flen, gw_mac, sta_mac, 0xC0A80101U, 0xC0A8018EU,
                        5555, 9000, (const uint8_t *)msg, (uint16_t)strlen(msg));
    zen_net_rx_frame(frame, flen);

    uint8_t rx_buf[64];
    uint32_t src_ip = 0;
    uint16_t src_port = 0;
    int32_t n = zen_udp_recvfrom(slot, rx_buf, sizeof(rx_buf), &src_ip, &src_port, 10);
    assert(n == (int32_t)strlen(msg));
    assert(src_ip == 0xC0A80101U && src_port == 5555);
    assert(memcmp(rx_buf, msg, (size_t)n) == 0);

    prev_tx = g_mock_tx_cnt;
    assert(zen_udp_sendto(9000, 0xC0A80101U, 5555, (const uint8_t *)"ACK", 3) == 0);
    assert(g_mock_tx_cnt == prev_tx + 1);
    assert(g_mock_tx_buf[23] == 17); // IP proto UDP
    zen_udp_unbind(slot);

    zen_net_stats_t st;
    zen_net_get_stats(&st);
    assert(st.arp_rx >= 1 && st.icmp_rx >= 1 && st.icmp_tx >= 1 && st.udp_rx >= 3 && st.checksum_drops == 0);

    printf("          ICMPv4 Echo Reply & 4-slot UDP bind/recvfrom/sendto verified. [PASS]\n");
}

// -------------------------------------------------------------------------
// Test 16: ZenNet Phase 3 Utilities (DNS RFC 1035 & SNTP UTC Formatting)
// -------------------------------------------------------------------------
static void test_zen_net_utils_dns_and_sntp(void) {
    printf("[TEST 16] Verifying ZenNet DNS query/response & SNTP UTC formatter...\n");

    // 1. Build RFC 1035 DNS A-record query for "pool.ntp.org"
    uint8_t q[128];
    size_t qlen = zen_dns_build_query("pool.ntp.org", 0x1234, q, sizeof(q));
    assert(qlen == 12 + 14 + 4);
    assert(q[0] == 0x12 && q[1] == 0x34);
    assert(q[12] == 4 && memcmp(q + 13, "pool", 4) == 0);
    assert(q[17] == 3 && memcmp(q + 18, "ntp", 3) == 0);
    assert(q[21] == 3 && memcmp(q + 22, "org", 3) == 0);
    assert(q[25] == 0);

    // 2. Build mock DNS response with compressed pointer (0xC0 0x0C) -> 162.159.200.1
    uint8_t resp[128];
    memcpy(resp, q, qlen);
    resp[2] = 0x81; resp[3] = 0x80; // QR=1, RD=1, RA=1, RCODE=0
    resp[6] = 0x00; resp[7] = 0x01; // ANCOUNT=1
    uint8_t ans[16] = {
        0xC0, 0x0C,             // Name pointer to offset 12
        0x00, 0x01,             // TYPE = A
        0x00, 0x01,             // CLASS = IN
        0x00, 0x00, 0x00, 0x3C, // TTL = 60s
        0x00, 0x04,             // RDLENGTH = 4
        162, 159, 200, 1        // 162.159.200.1
    };
    memcpy(resp + qlen, ans, sizeof(ans));
    uint32_t resolved_ip = 0;
    assert(zen_dns_parse_response(resp, qlen + sizeof(ans), 0x1234, &resolved_ip));
    assert(resolved_ip == 0xA29FC801U);
    assert(!zen_dns_parse_response(resp, qlen + sizeof(ans), 0x9999, &resolved_ip));

    // 3. Civil UTC timestamp formatter verification (known Unix epochs)
    char ts[32];
    zen_time_format_utc(0U, ts, sizeof(ts));
    assert(strcmp(ts, "1970-01-01 00:00:00 UTC") == 0);
    zen_time_format_utc(1709164800U, ts, sizeof(ts)); // 2024-02-29 00:00:00 UTC (leap day!)
    assert(strcmp(ts, "2024-02-29 00:00:00 UTC") == 0);

    // 4. Syslog config & send check (while ZenNet is up from Test 14/15)
    zen_syslog_config(0xC0A80101U, 514);
    assert(zen_syslog_is_enabled() && zen_syslog_get_port() == 514);
    assert(zen_syslog_send("Phase3-OK") == 0);
    zen_syslog_config(0, 514);
    assert(!zen_syslog_is_enabled());

    printf("          DNS RFC 1035, SNTP UTC leap-year formatter & UDP syslog verified. [PASS]\n");
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
    test_zen_net_arp_and_dhcp();
    test_zen_net_icmp_and_udp();
    test_zen_net_utils_dns_and_sntp();

    printf("====================================================\n");
    printf("  ALL 16 UNIT TESTS PASSED SUCCESSFULLY! (100%%)     \n");
    printf("====================================================\n");
    return 0;
}
