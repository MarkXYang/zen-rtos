# Building a Preemptive RTOS from Scratch on ARM Cortex-M

This tutorial walks through every layer required to build a production-grade, zero-dependency preemptive Real-Time Operating System (**ZenRTOS**) from bare silicon up to a multi-tasking Zen-C application on an **ARM Cortex-M4** microcontroller (**STM32L476RG**).

---

## 1. Architecture Overview: How an ARM Cortex-M RTOS Works

ARM Cortex-M processors (M0, M0+, M3, M4, M7, M33) were specifically designed in hardware to run an RTOS without assembly-heavy interrupt wrappers:

1. **Dual Stack Pointers (`MSP` & `PSP`)**:
   - **Main Stack Pointer (`MSP`)**: Used by the kernel at boot and inside all Interrupt Service Routines (ISRs / Handler Mode).
   - **Process Stack Pointer (`PSP`)**: Dedicated to user tasks running in Thread Mode (`CONTROL` register bit 1 = `1`). Each task gets its own isolated stack array in SRAM.
2. **Hardware Auto-Stacking on Exceptions**:
   - When an interrupt (`SysTick`, `PendSV`, UART, etc.) fires while a task is running on `PSP`, the Cortex-M hardware **automatically pushes 8 registers** (`R0`, `R1`, `R2`, `R3`, `R12`, `LR`, `PC`, `xPSR`) onto that task's `PSP` in 12 clock cycles.
3. **Delayed Context Switching via `PendSV`**:
   - Configured to the **lowest interrupt priority (`0xFF`)**, `PendSV` waits until all higher-priority hardware ISRs finish, then saves only the remaining 8 callee-saved registers (`R4–R11`), swaps the Task Control Block (`TCB`) pointer, pops the incoming task's `R4–R11`, updates `PSP`, and returns via `EXC_RETURN = 0xFFFFFFFD`.
4. **Periodic Timebase via `SysTick`**:
   - A built-in 24-bit countdown timer generates a `1000 Hz` (`1 ms`) heartbeat that wakes sleeping tasks and triggers `PendSV` when a higher-priority task becomes ready.

---

## 2. Bootstrapping: Linker Script (`bsp/stm32l476/link.ld`) & Startup (`bsp/stm32l476/startup.s`)

### 2.1 Memory Map & Linker Script
On the **STM32L476RG**, non-volatile **Flash (`1024 KB`)** starts at `0x08000000` and **SRAM1 (`96 KB`)** starts at `0x20000000` (top of stack `_estack = 0x20018000`).

```ld
MEMORY
{
    FLASH (rx)  : ORIGIN = 0x08000000, LENGTH = 1024K
    RAM   (rwx) : ORIGIN = 0x20000000, LENGTH = 96K
}

_estack = ORIGIN(RAM) + LENGTH(RAM); /* 0x20018000: Initial MSP top */

SECTIONS
{
    /* 1. Interrupt Vector Table MUST sit at 0x08000000 */
    .isr_vector : {
        . = ALIGN(4);
        KEEP(*(.isr_vector))
        . = ALIGN(4);
    } > FLASH

    /* 2. Code (.text) and Read-Only Constants (.rodata) in Flash */
    .text : {
        . = ALIGN(4);
        *(.text) *(.text*)
        *(.rodata) *(.rodata*)
        . = ALIGN(4);
        _etext = .;
    } > FLASH

    /* 3. Initialized globals (.data): stored in Flash (_sidata), copied to RAM at boot */
    _sidata = LOADADDR(.data);
    .data : {
        . = ALIGN(4);
        _sdata = .;
        *(.data) *(.data*)
        . = ALIGN(4);
        _edata = .;
    } > RAM AT > FLASH

    /* 4. Zero-initialized globals & task stacks (.bss) in RAM */
    .bss : {
        . = ALIGN(4);
        _sbss = .;
        *(.bss) *(.bss*) *(COMMON)
        . = ALIGN(4);
        _ebss = .;
    } > RAM
}
```

### 2.2 Vector Table & Reset Handler
At reset, the Cortex-M core loads `MSP` from `0x08000000` (`_estack`) and jumps to `Reset_Handler` at `0x08000004`, which copies `.data` from Flash to SRAM, zeroes `.bss`, and calls `main()`. Vector slots 14 (`0x08000038`) and 15 (`0x0800003C`) point to `PendSV_Handler` and `SysTick_Handler`.

---

## 3. Deep Dive: Thread / Task Management

### 3.1 Task Lifecycle & State Machine
Every thread (task) in ZenRTOS transitions deterministically between four states defined in `kernel/include/rtos.h`:

- **`TASK_STATE_READY`**: Task is runnable and its priority bit is set in `g_ready_bitmap`. The highest-priority `READY` task pointed to by `current_tcb` is actively **Running** on the CPU.
- **`TASK_STATE_BLOCKED`**: Task is waiting for a temporal event (`sleep_ticks > 0` via `rtos_delay()`) or a synchronization object (`sem->wait_list` or `mutex->wait_list`). Blocked tasks consume **zero CPU cycles**.
- **`TASK_STATE_SUSPENDED`**: Task is dormant or has exited cleanly via `rtos_task_exit_hook()` and is excluded from scheduling until re-initialized.

### 3.2 Anatomy of the Task Control Block (`tcb_t`)
Every task is tracked by a static `tcb_t` entry inside `g_tasks[RTOS_MAX_TASKS]`:

```c
typedef struct tcb {
    uint32_t           *sp;                  // Offset 0: Saved Process Stack Pointer (PSP)
    uint32_t            id;                  // Unique 1-based Task ID (1 = Idle Task)
    uint32_t            base_priority;       // Static priority assigned at creation (0..31)
    uint32_t            current_priority;    // Dynamic priority (boosted during Mutex PI)
    task_state_t        state;               // READY, BLOCKED, or SUSPENDED
    uint32_t            sleep_ticks;         // Remaining SysTick ticks before wakeup/timeout
    const char         *name;                // Human-readable name for UART/GDB inspection
    struct tcb         *next_blocked;        // Intrusive linked-list pointer for wait queues
    // Debug & profiling fields
    uint32_t           *stack_base;          // Bottom of stack buffer (0xDEADBEEF canary)
    uint32_t            stack_size;          // Stack capacity in 32-bit words
    uint32_t            run_ticks;           // Cumulative CPU ticks executed by this task
    uint32_t            ctx_switches;        // Total times scheduled onto the CPU
} tcb_t;
```

**Why `sp` Must Be at Offset 0**: In `PendSV_Handler`, the assembly routine loads `r2 = current_tcb` and executes `str r0, [r2]` to store the outgoing `PSP`, then loads `r4 = next_tcb` and executes `ldr r0, [r4]` to fetch the incoming `PSP`. Keeping `sp` at byte offset `0` decouples the assembly context switcher from any changes made to the rest of `tcb_t`.

### 3.3 Forging the Synthetic Cortex-M4 Stack Frame & AAPCS Alignment
When `rtos_task_create()` initializes a new task, it prepares the task's stack so `PendSV_Handler` can restore the task as if it had been preempted earlier:

1. **AAPCS 8-Byte Stack Alignment**: The ARM Architecture Procedure Call Standard (AAPCS) requires the stack pointer to be **8-byte aligned** (`sp & ~0x7`) at every public function boundary so 64-bit `LDRD`/`STRD` and floating-point operations never fault.
2. **Hardware Exception Frame (8 Words)**: Pushed at the very top of the aligned stack (`xPSR`, `PC`, `LR`, `R12`, `R3`, `R2`, `R1`, `R0`).
   - `xPSR = 0x01000000`: Sets **Bit 24 (Thumb State `T` bit)**. Without this bit, the Cortex-M4 immediately faults with an `INVSTATE` UsageFault when popping `PC`.
   - `PC = (uint32_t)func`: Entry point of the task function.
   - `LR = (uint32_t)rtos_task_exit_hook`: Safety net! If a user task accidentally returns from its `while(1)` loop, the CPU branches to `rtos_task_exit_hook()`, which suspends the task cleanly and reschedules instead of jumping to `0x00000000` and crashing.
   - `R0 = (uint32_t)arg`: Passes the user's `void *arg` parameter directly into the first argument register per AAPCS.
3. **Software Context Frame (8 Words)**: `R11` down to `R4` initialized to `0x00000000`, popped manually by `ldmia r0!, {r4-r11}` in `PendSV_Handler`.

---

## 4. Deep Dive: The RTOS Scheduler

### 4.1 Scheduling Policies: Preemptive Priority vs. Round-Robin vs. Cooperative
An RTOS scheduler decides which `TASK_STATE_READY` task gets the CPU next:

1. **Strict Priority Preemptive Scheduling (Implemented in ZenRTOS)**:
   - Priority `0` is the highest urgency; Priority `31` is the lowest (reserved for the `Idle` task).
   - Whenever a higher-priority task wakes up (e.g., in `SysTick_Handler`, `rtos_sem_signal()`, or `rtos_mutex_unlock()`), `rtos_schedule()` **immediately preempts** the running lower-priority task by pending `PendSV`.
2. **Cooperative Yielding (`rtos_yield`)**:
   - Tasks can voluntarily yield the CPU at any time via `rtos_yield()`.
3. **Round-Robin Time-Slicing (Equal-Priority Extension)**:
   - When multiple tasks share the same priority level $P$, round-robin scheduling rotates the starting search index `(last_scheduled_idx + 1) % g_task_count` after a configurable time quantum (e.g., every 10 ticks), ensuring fairness among equal-priority peers without starving higher-priority tasks.

### 4.2 How the $O(1)$ Priority Bitmap Works
Traditional schedulers iterate through all tasks (`for (i = 0; i < N; i++)`) to find the highest-priority ready task—an $O(N)$ operation that introduces jitter as task count grows.

ZenRTOS uses a 32-bit bitmask `g_ready_bitmap` where **Bit $P$ is `1` if and only if at least one task at priority $P$ is `READY`**:
- Setting a priority ready: `g_ready_bitmap |= (1UL << prio);`
- Clearing a priority when no tasks remain ready at `prio`: `g_ready_bitmap &= ~(1UL << prio);`
- Finding the highest ready priority (lowest bit index $0 \dots 31$):
  ```c
  uint32_t highest_prio = (uint32_t)__builtin_ctz(g_ready_bitmap);
  ```
  On ARM Cortex-M3/M4, `__builtin_ctz(x)` compiles to two single-cycle instructions (`rbit` + `clz`), guaranteeing **deterministic $O(1)$ priority resolution** regardless of how many priority levels are active.

### 4.3 The Idle Task (`rtos_idle_task`) & Power Management
An RTOS must **always** have at least one task ready to run so `current_tcb` and `PSP` are always valid. During `rtos_init()`, ZenRTOS automatically creates the `Idle` task (Task ID `1`) at Priority `31`:

```c
static void rtos_idle_task(void *arg) {
    (void)arg;
    while (1) {
        __asm volatile ("wfi"); // Wait For Interrupt: puts Cortex-M4 core into low-power sleep!
    }
}
```
When all application tasks are blocked waiting on timers or sensors, the scheduler switches to `Idle`, which executes `WFI` to gate the CPU clock until the next hardware interrupt (`SysTick`, UART, DMA, GPIO EXTI) wakes the core in microseconds.

---

## 5. Deep Dive: Inter-Thread Communication & Synchronization (IPC)

When multiple preemptive tasks share peripherals, memory buffers, or state variables, unsynchronized access causes **race conditions** and **torn reads/writes**.

### 5.1 Counting & Binary Semaphores (`rtos_sem_t`)
- **Signaling / Producer-Consumer Synchronization**: Initialize with `initial_count = 0`:
  ```c
  rtos_sem_t data_ready_sem;
  rtos_sem_init(&data_ready_sem, 0, 1); // Binary signaling semaphore
  ```
  The consumer task calls `rtos_sem_wait(&data_ready_sem, 1000)` and enters `TASK_STATE_BLOCKED` on `sem->wait_list`. When the producer task (or ISR) calls `rtos_sem_signal(&data_ready_sem)`, the kernel pops the waiting task from `sem->wait_list`, sets its state to `TASK_STATE_READY`, updates `g_ready_bitmap`, and calls `rtos_schedule()` to immediately preempt if the woken consumer has higher priority.
- **Resource Pool Counting**: Initialize with `initial_count = N, max_count = N` to manage $N$ identical hardware buffers or DMA channels.

### 5.2 Recursive Mutexes with Priority Inheritance (`rtos_mutex_t`)
Unlike a semaphore, a **Mutex** (`rtos_mutex_t`) has the concept of **Task Ownership** (`mutex->owner`):
1. **Recursive Locking**: If the task that already owns the mutex calls `rtos_mutex_lock()` again (e.g., inside a nested helper function), `mutex->lock_count` increments without deadlocking the task. It must call `rtos_mutex_unlock()` the same number of times.
2. **Defeating Unbounded Priority Inversion via Priority Inheritance**:
   - When high-priority Task H ($P = 2$) blocks on a mutex owned by low-priority Task L ($P = 20$), Task L's `current_priority` is immediately boosted to `2` (`TRACE_MUTEX_PI_BOOST`).
   - Medium-priority tasks ($P = 10$) are prevented from preempting Task L while it holds the lock.
   - As soon as Task L calls `rtos_mutex_unlock()`, its priority is restored to `20` and Task H immediately preempts and acquires the mutex.

### 5.3 Thread-Safe Message Queues (Ring Buffer + Semaphores)
To pass structured data payloads (such as sensor packets or UART frames) between tasks without shared-variable locks, combine a fixed-size circular ring buffer with two counting semaphores (`items_avail` and `slots_free`):

```c
typedef struct {
    uint32_t   *buffer;
    uint32_t    capacity;
    uint32_t    head;
    uint32_t    tail;
    rtos_sem_t  items_avail;
    rtos_sem_t  slots_free;
} rtos_queue_t;

static inline void rtos_queue_init(rtos_queue_t *q, uint32_t *buf, uint32_t capacity) {
    q->buffer = buf;
    q->capacity = capacity;
    q->head = 0;
    q->tail = 0;
    rtos_sem_init(&q->items_avail, 0, (int32_t)capacity);
    rtos_sem_init(&q->slots_free, (int32_t)capacity, (int32_t)capacity);
}

static inline bool rtos_queue_send(rtos_queue_t *q, uint32_t msg, uint32_t timeout) {
    if (!rtos_sem_wait(&q->slots_free, timeout)) return false;
    __asm volatile ("cpsid i" : : : "memory");
    q->buffer[q->head] = msg;
    q->head = (q->head + 1) % q->capacity;
    __asm volatile ("cpsie i" : : : "memory");
    rtos_sem_signal(&q->items_avail);
    return true;
}

static inline bool rtos_queue_recv(rtos_queue_t *q, uint32_t *out_msg, uint32_t timeout) {
    if (!rtos_sem_wait(&q->items_avail, timeout)) return false;
    __asm volatile ("cpsid i" : : : "memory");
    *out_msg = q->buffer[q->tail];
    q->tail = (q->tail + 1) % q->capacity;
    __asm volatile ("cpsie i" : : : "memory");
    rtos_sem_signal(&q->slots_free);
    return true;
}
```

---

## 6. Deep Dive: Interrupt Management on ARM Cortex-M

### 6.1 NVIC Priority Hierarchy & Why `PendSV` Must Be `0xFF`
On ARM Cortex-M, **numerically lower interrupt priority values mean higher hardware urgency** (`0x00` = highest urgency, `0xFF` = lowest urgency):

```text
Hardware Urgency
  Highest (0x00)  +-------------------------------------------------------+
                  | Zero-Latency Hardware ISRs (Motor PWM, High-Speed ADC)|
                  +-------------------------------------------------------+
  Medium  (0x80)  | RTOS-Aware Peripheral ISRs (UART RX, I2C, SPI, EXTI)  |
                  | SysTick_Handler (1000 Hz RTOS Tick)                   |
                  +-------------------------------------------------------+
  Lowest  (0xFF)  | PendSV_Handler (Context Switcher - SCB_SHPR3 = 0xFF)  |
                  +-------------------------------------------------------+
  Thread Mode     | User Tasks (Priority 0 .. Priority 31 Idle)           |
                  +-------------------------------------------------------+
```

**Why `PendSV` Must Be `0xFF`**: If a UART ISR is running on `MSP` and `SysTick` preempts it and triggers a high-priority `PendSV`, `PendSV` would attempt to context-switch to a Thread Mode task (`EXC_RETURN = 0xFFFFFFFD`) while a hardware ISR is still active on `MSP`! The Cortex-M4 hardware immediately detects this illegal return and fires a **UsageFault (`INVPC`)**. Setting `PendSV` to `0xFF` guarantees that `PendSV` **tail-chains** only after the very last nested ISR completes.

### 6.2 Critical Sections: `CPSID i` (`PRIMASK`) vs. `BASEPRI`
When the kernel modifies `g_ready_bitmap` or a wait list (`sem->wait_list`), it must prevent an ISR from racing on those pointers:

1. **Global Interrupt Masking (`PRIMASK` via `cpsid i` / `cpsie i`)**:
   - Used in `rtos_enter_critical()`. Executes in 1 clock cycle and blocks all configurable interrupts for the ~15 instructions required to update TCB pointers.
2. **Priority-Threshold Masking (`BASEPRI` on Cortex-M3/M4/M7)**:
   - Writing `0x80` to the `BASEPRI` register masks only interrupts with priority $\ge \text{0x80}$ (such as `SysTick`, `PendSV`, and RTOS-aware peripheral ISRs), while allowing ultra-fast hardware control loops at priority `0x00..0x70` to fire with **zero jitter** even inside an RTOS critical section:
   ```c
   static inline void rtos_mask_kernel_irqs(void) {
       uint32_t basepri = 0x80;
       __asm volatile ("msr basepri, %0" : : "r"(basepri) : "memory");
   }
   static inline void rtos_unmask_kernel_irqs(void) {
       uint32_t basepri = 0x00;
       __asm volatile ("msr basepri, %0" : : "r"(basepri) : "memory");
   }
   ```

### 6.3 Rules for ISR-Safe Kernel Calls
- **An ISR Must NEVER Block**: An ISR runs in Handler Mode on `MSP` and has no `tcb_t` of its own. Calling `rtos_delay()`, `rtos_sem_wait()`, or `rtos_mutex_lock()` inside an ISR is illegal.
- **Signaling from an ISR**: Calling `rtos_sem_signal()` from an ISR is completely safe! Because `rtos_schedule()` only sets the `PENDSVSET` bit in `SCB_ICSR` (rather than switching stacks inline), the ISR finishes running to completion on `MSP`, and the hardware automatically tail-chains into `PendSV_Handler` on ISR exit to switch directly into the newly woken high-priority task.

---

## 7. Deep Dive: Memory Management in an Embedded RTOS

### 7.1 Flash vs. SRAM Partitioning
In `build/zen_rtos.elf`, memory is strictly partitioned so RAM is conserved for task stacks and state:
- **Flash (`0x08000000`)**: Holds `.isr_vector`, all executable code (`.text`), string literals (`"Sensor"`, `"Telemetry"`, UART banners in `.rodata`), and the initial values of `.data`.
- **SRAM (`0x20000000`)**:
  - **Kernel `.bss` / `.data`**: `g_tasks[16]`, `g_trace_buffer[64]`, `g_fault_info`, `g_idle_stack[128]`.
  - **Application Task Stacks (`.bss`)**: `stack_sensor[256]`, `stack_telemetry[256]`, `stack_led[256]`.
  - **Main Stack (`MSP`)**: Grows downward from `_estack` (`0x20018000`), used exclusively by `Reset_Handler` and ISRs.

```text
0x20018000 (_estack) +---------------------------------------+
                     | Main Stack (MSP) - Used only by ISRs  |
                     |                  |                    |
                     |                  v                    |
                     |           [ Unused SRAM ]             |
                     +---------------------------------------+
                     | stack_telemetry[256] (PSP Task #3)    |
                     | stack_sensor[256]    (PSP Task #2)    |
                     | stack_led[256]       (PSP Task #4)    |
                     | g_idle_stack[128]    (PSP Task #1)    |
                     | g_tasks[16], g_trace_buffer[64]       |
0x20000000 (_sdata)  +---------------------------------------+
```

### 7.2 Why Safety-Critical RTOS Kernels Avoid `malloc()` / `free()`
Standard C heap allocators (`malloc`/`free`) are dangerous in real-time embedded systems for three reasons:
1. **Heap Fragmentation**: Over days or months of variable-sized allocations, free SRAM fragments into small non-contiguous holes, eventually causing an allocation failure in the field.
2. **Non-Deterministic Timing**: Searching free-lists takes unbounded $O(N)$ time, violating hard real-time deadlines.
3. **Code Size & Thread-Safety Overhead**: Pulling in `newlib` `malloc` adds kilobytes of Flash/RAM overhead and requires global heap locks.

### 7.3 Deterministic $O(1)$ Fixed-Block Memory Pool Allocator
When dynamic object allocation is required (e.g., allocating packet buffers for an MQTT or CAN bus stack), an RTOS uses an **$O(1)$ Fixed-Block Memory Pool** where free blocks form an intrusive singly-linked list inside the unused blocks themselves:

```c
typedef struct rtos_pool_block {
    struct rtos_pool_block *next;
} rtos_pool_block_t;

typedef struct {
    rtos_pool_block_t *free_list;
    uint32_t           block_size;
    uint32_t           free_count;
} rtos_mempool_t;

static inline void rtos_mempool_init(rtos_mempool_t *pool, void *memory,
                                     uint32_t block_size, uint32_t block_count) {
    pool->free_list  = NULL;
    pool->block_size = block_size;
    pool->free_count = block_count;
    uint8_t *ptr = (uint8_t *)memory;
    for (uint32_t i = 0; i < block_count; i++) {
        rtos_pool_block_t *blk = (rtos_pool_block_t *)(ptr + (i * block_size));
        blk->next = pool->free_list;
        pool->free_list = blk;
    }
}

static inline void *rtos_mempool_alloc(rtos_mempool_t *pool) {
    void *res = NULL;
    __asm volatile ("cpsid i" : : : "memory");
    if (pool->free_list != NULL) {
        rtos_pool_block_t *blk = pool->free_list;
        pool->free_list = blk->next;
        pool->free_count--;
        res = (void *)blk;
    }
    __asm volatile ("cpsie i" : : : "memory");
    return res; // Guaranteed O(1) allocation, zero fragmentation!
}

static inline void rtos_mempool_free(rtos_mempool_t *pool, void *ptr) {
    if (!ptr) return;
    __asm volatile ("cpsid i" : : : "memory");
    rtos_pool_block_t *blk = (rtos_pool_block_t *)ptr;
    blk->next = pool->free_list;
    pool->free_list = blk;
    pool->free_count++;
    __asm volatile ("cpsie i" : : : "memory");
}
```

### 7.4 Stack Sizing, Canary Watermarking & MPU Stack Guards
Because each task has its own dedicated `PSP` stack array, sizing stacks accurately and detecting overflows early is essential:
1. **Canary Watermarking (`0xDEADBEEF`)**:
   - `rtos_task_create()` fills all stack words with `0xDEADBEEF`.
   - `rtos_task_stack_free_words()` counts how many `0xDEADBEEF` words at the bottom of the stack remain untouched—giving you the exact **high-water mark** (e.g., `StackFree=868/1024B` on our live STM32L476RG telemetry task).
   - `rtos_check_stack_overflow()` checks `stack_base[0] != 0xDEADBEEF` and `sp < stack_base` on every tick.
2. **Hardware Memory Protection Unit (MPU) Stack Guard (Optional Cortex-M4 Enhancement)**:
   - On Cortex-M4 devices with an MPU (`0xE000ED90`), you can program a 32-byte `XN / No-Access` MPU region at `current_tcb->stack_base` during each context switch so that a stack overflow triggers an immediate, precise `MemManage_Handler` exception *before* it can corrupt an adjacent variable in SRAM.

---

## 8. Context Switching & Fault Analysis in Assembly (`arch/arm/cortex-m4/context.s`)

```armasm
PendSV_Handler:
    cpsid i                 /* 1. Disable interrupts during TCB swap */

    /* 2. Save outgoing task context */
    mrs   r0, psp           /* r0 = outgoing task's PSP (hardware already pushed R0-xPSR) */
    stmdb r0!, {r4-r11}     /* Push R4-R11 onto outgoing task's stack */

    ldr   r1, =current_tcb  /* r1 = &current_tcb */
    ldr   r2, [r1]          /* r2 = current_tcb */
    str   r0, [r2]          /* current_tcb->sp = r0 (offset 0 of tcb_t!) */

    /* 3. Switch current_tcb to next_tcb */
    ldr   r3, =next_tcb
    ldr   r4, [r3]          /* r4 = next_tcb */
    str   r4, [r1]          /* current_tcb = next_tcb */

    /* 4. Restore incoming task context */
    ldr   r0, [r4]          /* r0 = next_tcb->sp */
    ldmia r0!, {r4-r11}     /* Pop R4-R11 from incoming task's stack */
    msr   psp, r0           /* Update hardware PSP register */

    cpsie i                 /* 5. Re-enable interrupts */
    ldr   lr, =0xFFFFFFFD   /* EXC_RETURN: Return to Thread Mode using PSP */
    bx    lr                /* Hardware pops {R0-R3, R12, LR, PC, xPSR} from PSP */

/* Post-Mortem Fault Analyzer Entry */
HardFault_Handler:
MemManage_Handler:
BusFault_Handler:
UsageFault_Handler:
    cpsid i
    tst   lr, #4            /* Bit 2 of EXC_RETURN: 0 = MSP, 1 = PSP */
    ite   eq
    mrseq r0, msp
    mrsne r0, psp
    b     rtos_fault_capture
```

---

## 9. Multi-Tasking Application in Zen-C (`app/main.zc`)

```zc
import "rtos.h" as rtos;
import "stm32l476.h" as bsp;

def STACK_SIZE_WORDS = 256;

let stack_led: u32[256];
let stack_sensor: u32[256];
let stack_telemetry: u32[256];

let g_sensor_mutex: rtos::rtos_mutex_t;
let g_shared_temperature: u32 = 25;

fn task_led_blink(_arg: void*) {
    bsp::bsp_init_led();
    while true {
        bsp::bsp_toggle_led();
        rtos::rtos_delay(500);
    }
}

fn task_sensor_poll(_arg: void*) {
    while true {
        rtos::rtos_mutex_lock(&g_sensor_mutex, 100);
        g_shared_temperature = g_shared_temperature + 1;
        if g_shared_temperature > 45 {
            g_shared_temperature = 20;
        }
        rtos::rtos_mutex_unlock(&g_sensor_mutex);
        rtos::rtos_delay(200);
    }
}

fn task_telemetry_report(_arg: void*) {
    while true {
        rtos::rtos_mutex_lock(&g_sensor_mutex, 100);
        let temp: u32 = g_shared_temperature;
        rtos::rtos_mutex_unlock(&g_sensor_mutex);

        let _overflow_id: u32 = rtos::rtos_check_stack_overflow();
        bsp::bsp_debug_dump_status(temp);
        rtos::rtos_delay(1000);
    }
}

fn main() -> int {
    bsp::bsp_init_uart(4000000, 115200);
    bsp::bsp_uart_puts("\n[ZenRTOS] Booting on STM32L476RG (4 MHz MSI, 115200 8N1)...\n");

    rtos::rtos_init();
    rtos::rtos_mutex_init(&g_sensor_mutex);

    rtos::rtos_task_create(task_sensor_poll, NULL, 5, &stack_sensor[0], STACK_SIZE_WORDS, "Sensor");
    rtos::rtos_task_create(task_telemetry_report, NULL, 8, &stack_telemetry[0], STACK_SIZE_WORDS, "Telemetry");
    rtos::rtos_task_create(task_led_blink, NULL, 10, &stack_led[0], STACK_SIZE_WORDS, "BlinkLED");

    rtos::rtos_start(4000000, 1000);
    return 0;
}
```

---

## 10. Building, Testing, Flashing & Live Hardware Inspection

```bash
# 1. Run the 8-test host verification suite on Linux
make test

# 2. Transpile Zen-C and cross-compile bare-metal ARM Cortex-M4 ELF/BIN
make zenc && make build

# 3. Flash to STM32L476RG over ST-LINK SWD via OpenOCD
openocd -f interface/stlink.cfg -f target/stm32l4x.cfg \
  -c "program build/zen_rtos.elf verify reset exit"

# 4. Monitor live RTOS telemetry over ST-LINK Virtual COM Port
stty -F /dev/ttyACM0 115200 raw -echo && cat /dev/ttyACM0
```
