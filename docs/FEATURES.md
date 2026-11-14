# ZenRTOS: Feature Summary & Capabilities Document
## Preemptive Real-Time Operating System for STM32L476 (ARM Cortex-M4)

**Document Version:** 1.2  
**Target Architecture:** ARM Cortex-M4F (ARMv7E-M)  
**Primary Target Device:** STM32L476RG (80 MHz, 128 KB SRAM, 1 MB Flash)  
**Implementation Languages:** Zen-C (`.zc`), Thumb-2 Assembly (`.s`), Freestanding C  

---

## 1. Executive Summary

**ZenRTOS** is an ultra-lightweight, deterministic, preemptive real-time operating system engineered specifically for resource-constrained ARM Cortex-M microcontrollers. Authored in **Zen-C**, it combines the safety and ergonomics of modern systems languages with a zero-overhead footprint and deterministic execution bounds.

```
┌─────────────────────────────────────────────────────────────┐
│                       ZenRTOS Core                          │
│                                                             │
│  ┌──────────────────┐  ┌──────────────────┐  ┌───────────┐  │
│  │ O(1) Preemptive  │  │ Dual-Stack Model │  │  Lowest-  │  │
│  │ Priority Sched.  │  │   (MSP / PSP)    │  │ Priority  │  │
│  │ (32 Prior. Lvl)  │  │ (8-byte aligned) │  │  PendSV   │  │
│  └──────────────────┘  └──────────────────┘  └───────────┘  │
│                                                             │
│  ┌──────────────────┐  ┌──────────────────┐  ┌───────────┐  │
│  │  Bounded FIFO    │  │  Mutex + Priority│  │ Counting  │  │
│  │  Message Queues  │  │   Inheritance    │  │ & Binary  │  │
│  │ (rtos_queue_t)   │  │ (No Inv. Invert) │  │ Semaphore │  │
│  └──────────────────┘  └──────────────────┘  └───────────┘  │
│                                                             │
│  ┌──────────────────┐  ┌──────────────────┐  ┌───────────┐  │
│  │ Stack Canary &   │  │ 64-Event Trace   │  │ HardFault │  │
│  │ High-Watermarks  │  │   Ring Buffer    │  │  Analyzer │  │
│  └──────────────────┘  └──────────────────┘  └───────────┘  │
└─────────────────────────────────────────────────────────────┘
```

---

## 2. Core Kernel & Scheduling Features

### 2.1 Deterministic $O(1)$ Priority Scheduler
- **32 Discrete Priority Levels**: Priority `0` (highest / real-time) down to priority `31` (lowest / idle).
- **Single-Cycle Priority Resolution**: Active ready priorities are tracked in a 32-bit bitmask (`g_ready_bitmap`). Finding the highest-priority ready task executes in **1 CPU cycle** using the Cortex-M hardware `__builtin_ctz` (Count Trailing Zeros) instruction.

### 2.2 Dual-Stack Hardware Memory Isolation
- **MSP (Main Stack Pointer)**:
  - Used exclusively by the bootloader, Interrupt Service Routines (ISRs), and the kernel scheduler.
  - Guarantees that deep interrupt nesting never corrupts or overflows application task stacks.
- **PSP (Process Stack Pointer)**:
  - Each task executes on its own independent, statically allocated stack buffer in SRAM.
  - Stacks are strictly **8-byte aligned** to comply with the ARM Architecture Procedure Call Standard (AAPCS) and Cortex-M hardware requirements.

### 2.3 Sub-Microsecond Lowest-Priority Context Switch (`PendSV`)
- Context switches are deferred to the `PendSV` exception, configured to the **lowest possible interrupt priority (`0xFF`)**.
- **Zero Interrupt Jitter**: Time-critical peripheral interrupts (UART, SPI, ADC, CAN, Motor Timers) are never delayed by a context switch.
- Hand-optimized Thumb-2 assembly (`context.s`) executes register saving and restoring in **fewer than 25 CPU cycles**.

### 2.4 Low-Power Background Idle Task & Clean Task Exit Hook
- Automatically spawns an internal Idle Task at priority `31` that executes the ARM `wfi` (Wait For Interrupt) instruction whenever all user tasks are blocked.
- If a task function returns, the kernel automatically diverts execution to `rtos_task_exit_hook`, cleanly moving the task to `TASK_STATE_SUSPENDED`.

---

## 3. Task Management & Time Tracking

| Capability | API Function | Description |
| :--- | :--- | :--- |
| **System Initialization** | `rtos_init()` | Clears TCB table, initializes ready bitmask and trace buffer, and spawns the Idle task. |
| **Task Creation** | `rtos_task_create()` | Fills stack with `0xDEADBEEF` canaries, synthesizes an ARM exception frame (`xPSR`, `PC`, `LR`, `R0-R3, R12`, `R4-R11`), and enrolls the task. |
| **Kernel Start** | `rtos_start()` | Programs SysTick to 1 ms, configures PendSV priority, switches to PSP, and boots the highest-priority task. |
| **Non-Busy Sleep** | `rtos_delay(ticks)` | Puts the calling task into `TASK_STATE_BLOCKED`, yielding the CPU to lower-priority tasks. Wakes up precisely on tick match. |
| **Voluntary Yield** | `rtos_yield()` | Triggers `PendSV` to voluntarily yield the remainder of the current time slice. |
| **Time Query** | `rtos_get_tick()` | Returns the 32-bit monotonic system millisecond counter (`g_system_ticks`). |

---

## 4. Synchronization & Inter-Thread Communication (IPC)

### 4.1 Bounded Thread-Safe Message Queues (`rtos_queue_t`)
- **Zero-Allocation Circular FIFO Buffer**: Backed by a caller-provided static `uint32_t` storage array and two internal counting semaphores (`items_avail` and `slots_free`).
- **Blocking & Non-Blocking Send/Receive**:
  - `rtos_queue_init(&q, storage_buf, capacity)`: Initializes a bounded message queue.
  - `rtos_queue_send(&q, msg, timeout_ticks)`: Enqueues a 32-bit value or struct pointer in FIFO order. If the queue is full, the producer task blocks until a slot frees up or `timeout_ticks` expires. Automatically wakes any higher-priority consumer blocked on `rtos_queue_recv()`.
  - `rtos_queue_recv(&q, &out_msg, timeout_ticks)`: Dequeues the oldest message. If the queue is empty, the consumer task blocks until a producer sends a message or `timeout_ticks` expires.
  - `rtos_queue_count(&q)`: Returns the current number of queued messages.
- **Kernel Trace Integration**: Automatically records `TRACE_QUEUE_SEND` and `TRACE_QUEUE_RECV` events in `g_trace_buffer`.

#### Sample Code: Producer-Consumer Queue in Zen-C (`app/main.zc`)
```zc
import "rtos.h" as rtos;

def SENSOR_QUEUE_CAP = 8;
let g_sensor_queue: rtos::rtos_queue_t;
let g_sensor_qbuf: u32[8];

// Producer Task (Priority 5)
fn task_sensor_poll(_arg: void*) {
    let sample: u32 = 25;
    while true {
        sample = sample + 1;
        // Send temperature sample to consumer (wait up to 10 ticks if full)
        rtos::rtos_queue_send(&g_sensor_queue, sample, 10);
        rtos::rtos_delay(200);
    }
}

// Consumer Task (Priority 8)
fn task_telemetry_report(_arg: void*) {
    let latest_temp: u32 = 0;
    while true {
        // Drain all queued sensor samples
        while rtos::rtos_queue_count(&g_sensor_queue) > 0 {
            rtos::rtos_queue_recv(&g_sensor_queue, &latest_temp, 10);
        }
        rtos::rtos_delay(1000);
    }
}

fn main() -> int {
    rtos::rtos_init();
    rtos::rtos_queue_init(&g_sensor_queue, &g_sensor_qbuf[0], SENSOR_QUEUE_CAP);
    // ... create tasks and start scheduler ...
    return 0;
}
```

### 4.2 Mutex with Priority Inheritance (`rtos_mutex_t`)
- **Recursive Locking**: A task holding a mutex can re-lock it safely without deadlocking itself.
- **Priority Inversion Prevention**:
  - If a low-priority task holds a mutex and a high-priority task attempts to acquire it, the kernel **temporarily boosts the low-priority task's priority** to match the high-priority task (`TRACE_MUTEX_PI_BOOST`).
  - The moment the mutex is unlocked, the original owner's priority is restored, and the waiting high-priority task immediately unblocks and acquires the lock.

### 4.3 Counting & Binary Semaphores (`rtos_sem_t`)
- Configurable initial count and maximum token count.
- Supports non-blocking acquisition (`timeout_ticks = 0`) or blocking wait with automated sleep queue unblocking upon `rtos_sem_signal()`.

---

## 5. Debug, Profiling & Crash Analysis Subsystem

| Feature | API / Symbol | Description |
| :--- | :--- | :--- |
| **Stack High-Watermark** | `rtos_task_stack_free_words(id)` | Scans `0xDEADBEEF` (`RTOS_STACK_CANARY`) words from `stack_base` upward to measure minimum free stack space. |
| **Overflow Detector** | `rtos_check_stack_overflow()` | Verifies bottom canary word (`stack_base[0] == 0xDEADBEEF`) and `sp >= stack_base` on every `SysTick`. |
| **CPU & Task Profiler** | `rtos_get_stats(&stats)` | Computes total context switches, idle ticks, per-task `run_ticks`, and active CPU load percentage (`0..100%`). |
| **Circular Trace Buffer** | `g_trace_buffer[64]` | Records the last 64 kernel events (task switches, delays, wakeups, mutex PI boosts, queue send/recv, faults) in SRAM. |
| **HardFault Analyzer** | `g_fault_info` / `rtos_fault_capture()` | Extracts stacked `R0-R3, R12, LR, PC, xPSR` from `MSP`/`PSP` plus `CFSR, HFSR, MMFAR, BFAR` and offending task name. |
| **Live UART Telemetry** | `bsp_debug_dump_status()` | Streams live task table, stack watermarks, and CPU load over `USART2` (`PA2`/`PA3` ST-LINK VCP `/dev/ttyACM0` @ `115200 8N1`). |

---

## 6. Dual-Tier Verification & Test Infrastructure

### Tier 1: Host-Based Unit Test Suite (`make test`)
Executes 9 comprehensive unit tests natively on Linux (x86_64):
1. **Stack Frame Integrity Test**: Asserts 8-byte alignment, Thumb bit in `xPSR`, and argument placement.
2. **Priority Preemption Test**: Verifies that the highest priority ready task is always selected by the bitmask.
3. **Delay & SysTick Precision Test**: Asserts tasks stay blocked for exact tick counts and wake upon completion.
4. **Priority Inheritance Test**: Proves that a low-priority task holding a mutex is boosted and later reverted upon unlock.
5. **Semaphore Operation Test**: Verifies token depletion and FIFO wait-list signaling.
6. **Stack Canary Watermark & Overflow Test**: Verifies high-watermark word counts and corrupted bottom canary detection.
7. **Runtime Profiling & CPU Load Test**: Verifies per-task `run_ticks` and active CPU utilization percentage calculation.
8. **Circular Trace Buffer & HardFault Analyzer Test**: Verifies event ring buffer recording and Cortex-M4 fault frame extraction.
9. **Message Queue (`rtos_queue_t`) IPC Test**: Verifies FIFO message ordering, queue count tracking, and semaphore synchronization.
*(Status: **9/9 Unit Tests Passed — 100%**).*

### Tier 2: Physical Hardware In-Circuit Verification (STM32L476RG)
- Flashed and verified live on **NUCLEO-L476RG** via OpenOCD and ST-LINK V2-1.
- Confirmed live `USART2` telemetry streaming over `/dev/ttyACM0`, 1 Hz `PA5` LED toggling, and producer-consumer queue transfer between `Sensor` and `Telemetry` tasks.
