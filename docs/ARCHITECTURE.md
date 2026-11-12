# ZenRTOS: Architecture & Technical Design Document
## Preemptive Real-Time Operating System in Zen-C for STM32L476 (ARM Cortex-M4)

**Document Version:** 1.0  
**Target Hardware:** STM32L476xx (ARM Cortex-M4F @ 80 MHz, 128 KB SRAM, 1 MB Flash)  
**Primary Language:** Zen-C (`.zc`) with minimal Thumb-2 Assembly  
**Toolchain:** Zen-C Compiler (`zc`) + LLVM/Clang (`--target=arm-none-eabi`) or GCC (`arm-none-eabi-gcc`)  

---

## 1. System Overview & Core Objectives

**ZenRTOS** is an ultra-lightweight, deterministic, preemptive real-time operating system kernel designed specifically for ARM Cortex-M microcontrollers and authored in **Zen-C**.

### Key Architectural Tenets
1. **Zero Runtime Overhead**: No heap allocation required by the kernel core. All Task Control Blocks (TCBs) and stacks are statically determinable.
2. **Dual-Stack Hardware Isolation**: The kernel, exception handlers, and ISRs execute on the **Main Stack Pointer (MSP)**. User tasks execute exclusively on their individual **Process Stack Pointers (PSP)**.
3. **Lowest-Priority Deferred Context Switching**: Context switching is executed exclusively in the `PendSV` interrupt handler configured at the lowest possible priority (`0xFF`), guaranteeing that zero interrupt jitter is imposed on real-time hardware ISRs.
4. **Ergonomic Zen-C Abstraction**: Leverages Zen-C's modern typing, explicit bounds, and module structure while transpiling directly into strictly MISRA-compliant freestanding C.

---

## 2. Cortex-M4 Hardware Execution & Memory Model

```mermaid
flowchart TD
    subgraph Memory["STM32L476 Memory Layout (128 KB SRAM)"]
        direction TB
        RAM_BASE["0x20000000: SRAM1 Base (96 KB)"]
        KERNEL_DATA["Kernel Global Data & BSS (TCB Table)"]
        MSP_STACK["Main Stack (MSP) - Used by ISRs & Scheduler"]
        TASK1_STACK["Task 1 Stack (PSP 1)"]
        TASK2_STACK["Task 2 Stack (PSP 2)"]
        TASKN_STACK["Task N Stack (PSP N)"]
        RAM_END["0x2001FFFF: SRAM1 End"]
    end

    subgraph CoreRegisters["Cortex-M4 Registers"]
        R0_R12["R0 - R12: General Purpose"]
        SP_REG["SP (R13): Switched between MSP & PSP"]
        LR_REG["LR (R14): Link Register / EXC_RETURN"]
        PC_REG["PC (R15): Program Counter"]
        XPSR_REG["xPSR: Program Status Register"]
        FPU_REGS["S0 - S31: Floating Point Unit (Lazy Stacking)"]
    end

    subgraph Exceptions["Exception Controllers"]
        SYSTICK["SysTick (Periodic 1 ms Timer Tick)"]
        PENDSV["PendSV (Lowest Priority 0xFF Context Switch)"]
        SVC["SVC (Supervisor Call for System Boots)"]
    end

    SYSTICK -- "Time slice expired" --> PENDSV
    PENDSV -- "Swap PSP & TCB" --> SP_REG
```

### 2.1 Hardware Stack Frame vs. Software Stack Frame
When a context switch occurs, the processor saves half the registers automatically in hardware, and `PendSV` saves the remaining half in software:

```
+-----------------------------------------------------------+
| High Address                                              |
+-----------------------------------------------------------+
| xPSR          (Hardware saved - Thumb bit must be 1)      |
| PC            (Hardware saved - Task entry point)         |
| LR            (Hardware saved - Task exit stub / hook)    |
| R12           (Hardware saved)                            |
| R3            (Hardware saved)                            |
| R2            (Hardware saved)                            |
| R1            (Hardware saved)                            |
| R0            (Hardware saved - Task argument void *arg)  |
+-----------------------------------------------------------+ <--- Hardware Exception Entry
| R11           (Software saved by PendSV)                  |
| R10           (Software saved by PendSV)                  |
| R9            (Software saved by PendSV)                  |
| R8            (Software saved by PendSV)                  |
| R7            (Software saved by PendSV)                  |
| R6            (Software saved by PendSV)                  |
| R5            (Software saved by PendSV)                  |
| R4            (Software saved by PendSV)                  |
+-----------------------------------------------------------+ <--- Saved PSP in TCB
| Low Address (Stack Growth Downwards)                      |
+-----------------------------------------------------------+
```

---

## 3. Kernel Components & Data Structures

### 3.1 Task Control Block (TCB)
The `sp` pointer **must strictly reside at offset 0** of the structure to ensure single-cycle access from assembly (`ldr r0, [r1]`):

```zc
// zen-rtos/kernel/types.zc
enum TaskState {
    Ready,
    Blocked,
    Suspended
}

struct TCB {
    sp: *u32,             // Process Stack Pointer (Offset 0)
    id: u32,              // Unique task identifier
    priority: u32,        // 0 (Highest) to N (Lowest)
    state: TaskState,     // Current lifecycle state
    sleep_ticks: u32,     // Remaining SysTick count for delay
    stack_base: *u32,     // Lowest address of allocated stack buffer
    stack_size: u32,      // Total words allocated for stack canary check
    name: *const char     // Debugging label
}
```

### 3.2 System Call & Execution Flow
```mermaid
sequenceDiagram
    autonumber
    participant AppTask as Active Task (PSP)
    participant SysTick as SysTick Timer (MSP)
    participant PendSV as PendSV Interrupt (MSP)
    participant NextTask as Next Ready Task (PSP)

    AppTask->>AppTask: Running user code...
    SysTick->>SysTick: 1 ms Timer Fires
    SysTick->>SysTick: Decrement sleep_ticks for blocked tasks
    SysTick->>SysTick: Run Scheduler (Pick highest priority ready task)
    SysTick->>PendSV: Set PENDSVSET bit in SCB->ICSR
    SysTick-->>AppTask: SysTick exits (Cortex-M returns to task)

    Note over PendSV: All higher priority ISRs finish first
    PendSV->>AppTask: PendSV triggers (Hardware pushes R0-R3, R12, LR, PC, xPSR)
    PendSV->>PendSV: Software pushes R4-R11 to current PSP
    PendSV->>PendSV: current_tcb->sp = PSP
    PendSV->>PendSV: current_tcb = next_tcb
    PendSV->>PendSV: PSP = next_tcb->sp
    PendSV->>NextTask: Software pops R4-R11 from new PSP
    PendSV-->>NextTask: Exception returns (EXC_RETURN 0xFFFFFFFD)
    NextTask->>NextTask: Hardware auto-pops R0-R3, R12, LR, PC, xPSR & resumes!
```

---

## 4. Scheduler Design & Scheduling Invariants

1. **Preemptive Priority Scheduler with Round-Robin Aging**:
   - Tasks are indexed by priorities $0$ (highest) to $31$ (lowest).
   - If multiple tasks share the highest active priority, execution alternates in Round-Robin fashion upon each SysTick.
2. **Deterministic Time Complexity**:
   - Ready task lookup is $O(1)$ via a 32-bit priority bitmap (`__builtin_clz` count-leading-zeros single-cycle instruction).
3. **Idle Task Invariant**:
   - A dedicated lowest-priority Idle Task (`priority = 31`) is always running when all user tasks are asleep. It executes the `wfi` (Wait For Interrupt) low-power instruction to conserve battery.

---

## 5. Zen-C Language Integration Architecture

Zen-C compiles to freestanding C (`zc transpile --free`), enabling clean modular development while maintaining full control over microcontroller registers:

```
+-------------------------------------------------------------+
|                      User Application                       |
|           (Written in Zen-C: app.zc, sensors.zc)            |
+-------------------------------------------------------------+
                              |
+-------------------------------------------------------------+
|                      ZenRTOS Kernel                         |
|   • Task Lifecycle (task.zc)     • Queue / Sync (sync.zc)   |
|   • Priority Scheduler (sched.zc)• Timer Wheel (timer.zc)   |
+-------------------------------------------------------------+
                              |
+-------------------------------------------------------------+
|                   Hardware Abstraction                      |
|   • port_asm.s (PendSV context switch in ARM Assembly)       |
|   • port_c.zc (SysTick, NVIC, SCB register configuration)   |
|   • stm32l476.zc (RCC, GPIO, USART register maps)           |
+-------------------------------------------------------------+
```
