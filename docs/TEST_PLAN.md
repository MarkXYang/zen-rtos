# ZenRTOS: Test Plan & Test Program Specification
## Quality Assurance & Verification Suite for STM32L476 RTOS

**Document Version:** 1.0  

---

## 1. Testing Strategy: Dual-Tier Architecture

To achieve rapid iteration and high reliability, ZenRTOS uses a **dual-tier testing architecture**:

```
+-------------------------------------------------------------+
|                 Tier 1: Host-Based Mock Tests               |
|      (Runs natively on Linux PC via standard compiler)      |
|  • Scheduler state transitions      • Semaphore FIFO queues |
|  • Mutex priority inheritance logic • Ring buffer queues    |
+-------------------------------------------------------------+
                              ▲
                              │
+-------------------------------------------------------------+
|                Tier 2: Target Hardware Tests                |
|           (Runs on STM32L476 / QEMU ARM Cortex-M4)          |
|  • 8-byte stack alignment checks    • SysTick timer jitter  |
|  • PendSV register preservation     • Real ISR preemption   |
+-------------------------------------------------------------+
```

---

## 2. Test Cases & Test Matrix

| Test ID | Name | Objective | Pass Criteria |
| :--- | :--- | :--- | :--- |
| **TC-01** | Stack Frame Integrity | Verify initial task stack frame synthesis (`xPSR`, `PC`, `LR`, `R0-R12`, `R4-R11`). | SP is 8-byte aligned; `xPSR[24] == 1` (Thumb); initial `R0` equals task argument. |
| **TC-02** | Round-Robin Preemption | Verify that two equal-priority tasks share CPU time slices. | Task A and Task B execution counters increment in interleaved balance over $1000$ ticks. |
| **TC-03** | `rtos_delay()` Determinism | Verify that a task sleeping for $100$ ticks wakes up accurately. | Elapsed SysTicks upon return equals expected sleep window ($100 \pm 1$ tick). |
| **TC-04** | Strict Priority Preemption | Verify that a higher-priority task immediately preempts a lower-priority task upon waking. | High-priority task executes without waiting for current time-slice to expire. |
| **TC-05** | Mutex Priority Inheritance | Prevent priority inversion when a low-priority task holds a mutex wanted by a high-priority task. | Low-priority task temporarily inherits high priority until `mutex_unlock()`. |
| **TC-06** | Message Queue FIFO | Verify reliable inter-task data passing without data corruption or lost packets. | 1,000 sequential integers sent by producer match received stream exactly. |

---

## 3. Test Programs & Test Harness Implementation

### 3.1 Tier 1: Host Scheduler Simulation Harness (`tests/host/test_sched.c`)
Tests the scheduler logic on Linux without needing the ARM core:
```c
#include <stdio.h>
#include <assert.h>
#include "kernel.h"

void test_scheduler_round_robin(void) {
    printf("[TEST] Running test_scheduler_round_robin...\n");
    rtos_init();
    rtos_task_create(dummy_task1, 0, 10);
    rtos_task_create(dummy_task2, 0, 10);

    // Simulate 100 clock ticks
    for (int tick = 0; tick < 100; tick++) {
        SysTick_Handler();
    }

    assert(task1_counter > 40 && task2_counter > 40);
    printf("[PASS] Equal priority tasks executed concurrently.\n");
}
```

### 3.2 Tier 2: Bare-Metal Cortex-M4 Integration Test (`tests/target/test_preempt.zc`)
Executed on the STM32L476 hardware:
- **Task 1** (Priority 5): Blinks an LED and checks timer jitter.
- **Task 2** (Priority 2, Higher): Periodically interrupts Task 1 every 200 ms to log a high-precision timestamp to UART.
- **Assertion Monitor**: If any register corruption or stack overflow occurs, the kernel halts and triggers an LED error strobe (PB3).
