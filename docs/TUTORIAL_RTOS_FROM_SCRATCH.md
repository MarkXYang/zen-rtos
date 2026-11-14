# Building a Preemptive RTOS from Scratch on ARM Cortex-M

This tutorial walks through every layer required to build a production-grade, zero-dependency preemptive Real-Time Operating System (**ZenRTOS**) from bare silicon up to a multi-tasking Zen-C application on an **ARM Cortex-M4** microcontroller (**STM32L476RG**).

---

## Architecture Overview: How an ARM Cortex-M RTOS Works

ARM Cortex-M processors (M0, M0+, M3, M4, M7, M33) were specifically designed in hardware to run an RTOS without assembly-heavy interrupt wrappers:

1. **Dual Stack Pointers (`MSP` & `PSP`)**:
   - **Main Stack Pointer (`MSP`)**: Used by the kernel at boot and inside all Interrupt Service Routines (ISRs / Handler Mode).
   - **Process Stack Pointer (`PSP`)**: Dedicated to user tasks running in Thread Mode (`CONTROL` register bit 1 = `1`). Each task gets its own isolated stack array in SRAM.
2. **Hardware Auto-Stacking on Exceptions**:
   - When an interrupt (`SysTick`, `PendSV`, UART, etc.) fires while a task is running on `PSP`, the Cortex-M hardware **automatically pushes 8 registers** (`R0`, `R1`, `R2`, `R3`, `R12`, `LR`, `PC`, `xPSR`) onto that task's `PSP`.
3. **Delayed Context Switching via `PendSV`**:
   - Configured to the **lowest interrupt priority (`0xFF`)**, `PendSV` waits until all higher-priority hardware ISRs finish, then saves only the remaining 8 callee-saved registers (`R4–R11`), swaps the Task Control Block (`TCB`) pointer, pops the incoming task's `R4–R11`, updates `PSP`, and returns via `EXC_RETURN = 0xFFFFFFFD`.
4. **Periodic Timebase via `SysTick`**:
   - A built-in 24-bit countdown timer generates a `1000 Hz` (`1 ms`) heartbeat that wakes sleeping tasks and triggers `PendSV` when a higher-priority task becomes ready.

---

## Step 1: Memory Layout & Linker Script (`bsp/stm32l476/link.ld`)

Before a single instruction executes, the linker must know where non-volatile **Flash** and volatile **SRAM** reside in the ARM Cortex-M4 32-bit address space. On the **STM32L476RG**:
- **Flash (`1024 KB`)** starts at `0x08000000`
- **SRAM1 (`96 KB`)** starts at `0x20000000` (top of stack `_estack = 0x20018000`)

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

---

## Step 2: Vector Table & Bare-Metal Startup (`bsp/stm32l476/startup.s`)

At power-on or reset, the ARM Cortex-M core hardware reads two 32-bit words from address `0x08000000`:
1. Word `0` (`0x08000000`): Initial value of the **Main Stack Pointer (`MSP`)** (`_estack`).
2. Word `1` (`0x08000004`): Address of `Reset_Handler` (with Bit 0 set to `1` for Thumb state).
3. Words `14` & `15` (`0x08000038` & `0x0800003C`): Vectors for `PendSV_Handler` and `SysTick_Handler`.

```armasm
Reset_Handler:
    ldr   sp, =_estack          /* 1. Initialize MSP to top of SRAM (0x20018000) */

    /* 2. Copy initialized .data variables from Flash (_sidata) to SRAM (_sdata.._edata) */
    ldr   r0, =_sdata
    ldr   r1, =_edata
    ldr   r2, =_sidata
    movs  r3, #0
    b     LoopCopyDataInit
CopyDataInit:
    ldr   r4, [r2, r3]
    str   r4, [r0, r3]
    adds  r3, r3, #4
LoopCopyDataInit:
    adds  r4, r0, r3
    cmp   r4, r1
    bcc   CopyDataInit

    /* 3. Zero-fill .bss section (_sbss.._ebss) */
    ldr   r2, =_sbss
    ldr   r4, =_ebss
    movs  r3, #0
    b     LoopFillZerobss
FillZerobss:
    str   r3, [r2]
    adds  r2, r2, #4
LoopFillZerobss:
    cmp   r2, r4
    bcc   FillZerobss

    /* 4. Enter C/Zen-C main() */
    bl    main
```

---

## Step 3: Designing the Task Control Block (`tcb_t`) & Synthetic Stack Frame

### 3.1 The Task Control Block (`kernel/include/rtos.h`)
Place `uint32_t *sp` as the **very first field (byte offset `0`)** of `tcb_t` so `PendSV_Handler` can load and store the stack pointer directly via `[r2]` without offset math:

```c
typedef struct tcb {
    uint32_t           *sp;                  // Offset 0: Current Process Stack Pointer (PSP)
    uint32_t            id;                  // Task ID (1-based)
    uint32_t            base_priority;       // Assigned priority (0 = highest, 31 = Idle)
    uint32_t            current_priority;    // Dynamic priority (boosted during mutex PI)
    task_state_t        state;               // READY, BLOCKED, SUSPENDED
    uint32_t            sleep_ticks;         // Countdown ticks for rtos_delay / timeouts
    const char         *name;                // Debug task name
    struct tcb         *next_blocked;        // Intrusive wait-queue pointer
    uint32_t           *stack_base;          // Lowest stack address (0xDEADBEEF canary bottom)
    uint32_t            stack_size;          // Stack size in 32-bit words
    uint32_t            run_ticks;           // CPU runtime ticks
    uint32_t            ctx_switches;        // Number of times scheduled in
} tcb_t;
```

### 3.2 Forging the Initial Cortex-M4 Stack Frame
When a task is created in `rtos_task_create()`, it has never executed before—yet `PendSV_Handler` expects to pop 16 registers (`R4–R11` in software, followed by `R0–R3, R12, LR, PC, xPSR` in hardware). We synthesize that exact 16-word (64-byte) frame at the top of the task's stack buffer:

```text
High Memory (Top of stack_buf, 8-byte aligned)
+-----------------------------------+
| xPSR = 0x01000000 (Thumb Bit 24!) | <-- sp[15]  \
| PC   = (uint32_t)task_func        | <-- sp[14]   |
| LR   = (uint32_t)task_exit_hook   | <-- sp[13]   |
| R12  = 0x00000000                 | <-- sp[12]   | Popped automatically by
| R3   = 0x00000000                 | <-- sp[11]   | Cortex-M4 hardware on
| R2   = 0x00000000                 | <-- sp[10]   | bx 0xFFFFFFFD
| R1   = 0x00000000                 | <-- sp[9]    |
| R0   = (uint32_t)arg              | <-- sp[8]   /
+-----------------------------------+
| R11  = 0x00000000                 | <-- sp[7]   \
| ...                               |              | Popped manually by
| R4   = 0x00000000                 | <-- sp[0]   /  ldmia r0!, {r4-r11}
+-----------------------------------+ <-- t->sp saved here
| 0xDEADBEEF (Canary words...)      |
+-----------------------------------+
Low Memory (stack_base = &stack_buf[0])
```

**Critical Hardware Rule**: Bit 24 of `xPSR` (`0x01000000`) is the **Thumb State (`T`) bit**. If left `0`, the Cortex-M CPU faults with an `INVSTATE` UsageFault/HardFault on the first instruction!

---

## Step 4: Context Switching in Assembly (`arch/arm/cortex-m4/context.s`)

### 4.1 Launching the First Task (`rtos_start_first_task`)
```armasm
rtos_start_first_task:
    /* r0 = current_tcb->sp */
    ldmia r0!, {r4-r11}     /* 1. Pop initial software frame (R4-R11) */
    msr   psp, r0           /* 2. Set PSP to point at hardware frame (R0-xPSR) */
    mov   r1, #2            /* 3. CONTROL bit 1 = 1 -> Use PSP in Thread Mode */
    msr   control, r1
    isb                     /* 4. Instruction Synchronization Barrier */
    cpsie i                 /* 5. Enable interrupts */
    pop   {r0-r3, r12, lr}  /* 6. Pop R0 (arg), R1-R3, R12, LR (exit hook) */
    pop   {pc}              /* 7. Jump straight into task entry function! */
```

### 4.2 The `PendSV_Handler` Context Switcher
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
```

---

## Step 5: $O(1)$ Priority Scheduler & `SysTick` Timebase (`kernel/rtos.c`)

1. **$O(1)$ Priority Bitmap**: `g_ready_bitmap` uses bit $N$ for priority $N$ ($0 = \text{highest}, 31 = \text{lowest}$). `__builtin_ctz(g_ready_bitmap)` resolves the highest ready priority in a single hardware instruction (`rbit` + `clz`).
2. **Interrupt Priorities**: `PendSV` is set to lowest priority (`0xFF` in `SCB_SHPR3`) so context switches only occur after all hardware ISRs complete.
3. **1000 Hz `SysTick`**: `SYST_RVR = (cpu_freq_hz / tick_freq_hz) - 1` generates a 1 ms periodic interrupt that updates `g_system_ticks`, checks stack canaries, decrements `sleep_ticks`, and calls `rtos_schedule()`.

---

## Step 6: Synchronization & Priority Inheritance Mutexes

To prevent unbounded priority inversion:
- When a high-priority task blocks on a `rtos_mutex_t` held by a lower-priority task, `rtos_mutex_lock()` boosts `mutex->owner->current_priority` to match the high-priority caller and updates `g_ready_bitmap`.
- When the owner calls `rtos_mutex_unlock()`, its priority is restored to `mutex->original_priority`, ownership transfers to the highest-priority waiting task, and `rtos_schedule()` immediately switches context.

---

## Step 7: Debugging & Observability (`kernel/rtos.c` & `bsp/stm32l476/stm32l476.h`)

1. **Stack Canary Watermarks**: `0xDEADBEEF` fill in `rtos_task_create()`, scanned by `rtos_task_stack_free_words()` and checked every tick by `rtos_check_stack_overflow()`.
2. **Circular Trace Buffer**: 64-entry `g_trace_buffer` in SRAM recording task switches, wakeups, mutex locks, priority boosts, and faults.
3. **HardFault Post-Mortem Analyzer**: `HardFault_Handler` inspects `LR` bit 2 (`EXC_RETURN`) to extract the faulting frame from `MSP` or `PSP` into `g_fault_info` (`PC`, `LR`, `CFSR`, `HFSR`, `MMFAR`, `BFAR`, and offending task).
4. **Live `USART2` Serial Telemetry**: `bsp_debug_dump_status()` streams task runtime statistics and stack watermarks over `/dev/ttyACM0` (`115200 8N1`).

---

## Step 8: Building, Testing, and Flashing

```bash
# 1. Run host unit tests on Linux
make test

# 2. Transpile Zen-C and compile bare-metal STM32L476 ELF & binary
make zenc && make build

# 3. Flash to STM32L476RG via OpenOCD
openocd -f interface/stlink.cfg -f target/stm32l4x.cfg \
  -c "program build/zen_rtos.elf verify reset exit"

# 4. Read live serial telemetry from ST-LINK Virtual COM Port
stty -F /dev/ttyACM0 115200 raw -echo && cat /dev/ttyACM0
```
