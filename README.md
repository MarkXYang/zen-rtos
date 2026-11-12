# ZenRTOS
## Ultra-Lightweight Preemptive Real-Time Operating System in Zen-C for STM32L476 (ARM Cortex-M4)

ZenRTOS is a zero-overhead, deterministically scheduled real-time operating system written in **Zen-C** and compiled for the **STM32L476RG** microcontroller (ARM Cortex-M4F @ 80 MHz, 128 KB SRAM, 1 MB Flash).

---

## 🌟 Key Features

- **Dual-Stack Hardware Isolation**:
  - **MSP (Main Stack Pointer)**: Dedicated to ISRs, exception handlers (`SysTick`, `PendSV`), and boot initialization.
  - **PSP (Process Stack Pointer)**: Each task executes on its own independent stack in SRAM with 8-byte alignment.
- **Sub-microsecond Lowest-Priority Context Switch**:
  - Context switching runs inside `PendSV` configured at lowest priority (`0xFF`), eliminating interrupt latency for critical hardware ISRs.
- **Strict Priority Preemptive Scheduler ($O(1)$)**:
  - 32 discrete priority levels ($0$ = highest, $31$ = idle task).
  - Single-cycle priority resolution using `__builtin_ctz` hardware instruction.
- **Real-Time Synchronization Primitives**:
  - **Binary & Counting Semaphores**: Wait list blocking and non-blocking token acquisition.
  - **Mutex with Priority Inheritance**: Automatically prevents priority inversion by temporarily boosting the lock owner's priority.
- **Ultra-Compact Footprint**:
  - Total kernel binary size: **~3.4 KB Flash** and **~4 KB RAM** (including 3 task stacks and TCBs).
- **Dual-Tier Test Suite**:
  - Host-based unit test harness running natively on Linux.
  - Bare-metal target execution on STM32L476 hardware or QEMU.

---

## 📁 Repository Structure

```
zen-rtos/
├── docs/                      # Architectural & Engineering Specifications
│   ├── ARCHITECTURE.md        # Deep dive into Cortex-M4 context switching & memory model
│   ├── IMPLEMENTATION_PLAN.md # Staged 5-phase engineering roadmap
│   └── TEST_PLAN.md           # Dual-tier QA test plan & test matrix
│
├── kernel/                    # ZenRTOS Core Kernel
│   ├── include/
│   │   └── rtos.h             # Public RTOS API, TCB definitions, Mutex/Semaphore structures
│   └── rtos.c                 # Preemptive scheduler, sleep queues, and sync primitives
│
├── arch/arm/cortex-m4/        # Architecture Port Layer
│   └── context.s              # PendSV_Handler Thumb-2 assembly context switch
│
├── bsp/stm32l476/             # Board Support Package for NUCLEO-L476RG
│   ├── link.ld                # Linker script (Flash: 1024 KB @ 0x08000000, SRAM: 96 KB @ 0x20000000)
│   ├── startup.s              # Cortex-M4 Vector Table & Reset_Handler
│   └── stm32l476.h            # Hardware registers (RCC, GPIOA LED PA5, GPIOC Button PC13)
│
├── app/                       # Multi-task Demonstration Application
│   ├── main.zc                # High-level task implementation written in Zen-C
│   └── main.c                 # Reference C implementation
│
├── tests/
│   └── host/
│       └── test_scheduler.c   # 5 automated unit test cases (runs on Linux)
│
└── Makefile                   # Build, test, and transpilation automation
```

---

## 🚀 Quickstart & Commands

### 1. Run Host Unit Tests (Linux)
Test stack frame synthesis, priority scheduling, sleep queue wakeups, semaphore signaling, and mutex priority inheritance without needing physical hardware:

```bash
cd ~/projects/zen-rtos
make test
```

Expected output:
```
--- Executing ZenRTOS Host Test Suite ---
====================================================
      ZenRTOS Host Verification Test Suite         
====================================================
[TEST 1] Verifying task stack initialization and alignment...
         Stack frame integrity verified. [PASS]
[TEST 2] Verifying strict priority preemptive scheduler...
         Highest priority task (Priority 3) selected. [PASS]
[TEST 3] Verifying rtos_delay() and SysTick wakeup...
         Task unblocked precisely on 5th SysTick. [PASS]
[TEST 4] Verifying mutex priority inheritance...
         LowPrio task boosted to Priority 2 via Inheritance. [PASS]
         LowPrio task restored to base priority 20 upon unlock. [PASS]
[TEST 5] Verifying counting and binary semaphore...
         Semaphore operations verified. [PASS]
====================================================
  ALL 5 UNIT TESTS PASSED SUCCESSFULLY! (100%)      
====================================================
```

### 2. Transpile Zen-C Application
Compile `app/main.zc` to pure C using the Zen-C compiler:
```bash
make zenc
```

### 3. Build Bare-Metal Binary for STM32L476
Compile the complete bare-metal operating system using `clang` + `ld.lld`:
```bash
make build
```
Outputs:
- `build/zen_rtos.elf` (ELF executable for debugging with GDB / OpenOCD)
- `build/zen_rtos.bin` (Raw 3.4 KB binary ready to flash)

### 4. Flash to NUCLEO-L476RG
Connect your STM32L476 board via USB and flash using OpenOCD or ST-Link:
```bash
openocd -f interface/stlink.cfg -f target/stm32l4x.cfg -c "program build/zen_rtos.elf verify reset exit"
```
Or copy `build/zen_rtos.bin` directly onto the mounted `NODE_L476RG` USB mass-storage drive!
