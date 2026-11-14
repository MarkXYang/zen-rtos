# ZenRTOS Debugging & Observability Guide

## 1. Summary: Previous vs. Enhanced Debug Status

| Capability | Previous Status (v1.0) | Enhanced Status (v1.1) |
| :--- | :--- | :--- |
| **Stack Overflow & Watermark** | Raw stack buffer allocation; no canary fill or overflow detection. | Full `0xDEADBEEF` (`RTOS_STACK_CANARY`) stack initialization, per-tick hardware canary verification (`rtos_check_stack_overflow`), and high-water mark calculation (`rtos_task_stack_free_words`). |
| **Task Runtime Profiling** | Only `id`, `name`, `state`, and `priority` in `tcb_t`. | Per-task `run_ticks`, `ctx_switches`, `stack_base`, `stack_size`, plus global CPU load percentage (`rtos_get_stats`). |
| **Kernel Event Tracing** | None. | 64-entry circular trace ring buffer (`g_trace_buffer`) in SRAM capturing timestamped task switches, delays, wakeups, semaphore/mutex operations, priority inheritance boosts, overflows, and faults. |
| **Crash / HardFault Analyzer** | Weak `Default_Handler` infinite loop; no register or fault capture. | Assembly `HardFault_Handler` / `MemManage` / `BusFault` / `UsageFault` stub inspects `EXC_RETURN` (`MSP` vs `PSP`) and populates `g_fault_info` (`PC`, `LR`, `R0-R3`, `R12`, `xPSR`, `CFSR`, `HFSR`, `MMFAR`, `BFAR`, and offending task). |
| **Kernel Assertions** | Host-only `assert()` in unit tests. | Bare-metal `RTOS_ASSERT(expr)` macro recording `__FILE__`, `__LINE__`, and offending task into `g_fault_info` and `g_trace_buffer`. |
| **Live Serial Console (`USART2`)** | Register macros defined, but no driver or output. | Bare-metal `USART2` (`PA2` TX / `PA3` RX, `AF7`, `115200 8N1`) wired to ST-LINK Virtual COM Port (`/dev/ttyACM0`) with `bsp_debug_dump_status()` streaming live telemetry every second. |

---

## 2. Debug Architecture & Features

### 2.1 Stack High-Watermark & Overflow Detector
- **Canary Initialization**: [`rtos_task_create()`](../kernel/rtos.c) fills every word of a task's stack with `0xDEADBEEF` (`RTOS_STACK_CANARY`) before constructing the initial Cortex-M4 stack frame.
- **High-Water Mark Inspection**: [`rtos_task_stack_free_words()`](../kernel/rtos.c) scans upward from `t->stack_base` to count untouched canary words, reporting the exact minimum free stack bytes ever reached by each task.
- **Real-Time Overflow Guard**: [`SysTick_Handler()`](../kernel/rtos.c) and [`rtos_check_stack_overflow()`](../kernel/rtos.c) verify that `stack_base[0] == 0xDEADBEEF` and `sp >= stack_base`. If violated, `g_stack_overflow_task_id` is latched and a `TRACE_STACK_OVERFLOW` event is recorded.

### 2.2 Circular Kernel Trace Ring Buffer (`g_trace_buffer`)
- **Zero-Allocation Ring Buffer**: Stores the most recent `64` kernel events (`RTOS_TRACE_BUFFER_SIZE`) in `g_trace_buffer` (`12` bytes per entry: `tick`, `type`, `task_id`, `arg1`, `arg2`).
- **Traced Events** (`rtos_trace_type_t` in [`kernel/include/rtos.h`](../kernel/include/rtos.h)):
  - `TRACE_TASK_CREATE` (`1`), `TRACE_TASK_SWITCH` (`2`), `TRACE_TASK_DELAY` (`3`), `TRACE_TASK_WAKE` (`4`)
  - `TRACE_SEM_WAIT` (`5`), `TRACE_SEM_SIGNAL` (`6`)
  - `TRACE_MUTEX_LOCK` (`7`), `TRACE_MUTEX_UNLOCK` (`8`), `TRACE_MUTEX_PI_BOOST` (`9`)
  - `TRACE_STACK_OVERFLOW` (`10`), `TRACE_FAULT` (`11`), `TRACE_ASSERT_FAIL` (`12`)

### 2.3 Cortex-M4 HardFault & Assertion Post-Mortem Analyzer (`g_fault_info`)
- **Dual-Stack Exception Extraction**: In [`arch/arm/cortex-m4/context.s`](../arch/arm/cortex-m4/context.s), `HardFault_Handler`, `MemManage_Handler`, `BusFault_Handler`, and `UsageFault_Handler` test Bit 2 of `LR` (`EXC_RETURN`) using `tst lr, #4` to select either `MSP` or `PSP` and pass the faulting stack frame to [`rtos_fault_capture()`](../kernel/rtos.c).
- **Captured Registers**: Extracts stacked `R0`, `R1`, `R2`, `R3`, `R12`, `LR`, `PC`, `xPSR` alongside Cortex-M4 System Control Block registers `SCB_CFSR` (`0xE000ED28`), `SCB_HFSR` (`0xE000ED2C`), `SCB_MMFAR` (`0xE000ED34`), and `SCB_BFAR` (`0xE000ED38`), plus `offending_task_id` and `offending_task_name`.

### 2.4 Live ST-LINK Virtual COM Port (`USART2`) Telemetry Inspector
- Configured in [`bsp/stm32l476/stm32l476.h`](../bsp/stm32l476/stm32l476.h) via `bsp_init_uart(4000000, 115200)` on `PA2` (`USART2_TX`) and `PA3` (`USART2_RX`).
- `bsp_debug_dump_status()` prints a live summary table every `1000 ms`:
  ```text
  === [ZenRTOS Debug Telemetry] Tick: 20657 ms | CPU Load: 3% | CtxSw: 325 | Temp: 25C ===
    Task #1 [Idle] Prio=31 RunTicks=19999 CtxSw=155 StackFree=444/512B
    Task #2 [Sensor] Prio=5 RunTicks=0 CtxSw=104 StackFree=908/1024B
    Task #3 [Telemetry] Prio=8 RunTicks=682 CtxSw=25 StackFree=868/1024B
    Task #4 [BlinkLED] Prio=10 RunTicks=0 CtxSw=41 StackFree=916/1024B
  ```

---

## 3. How to Use the Debug Tools on Hardware

### 3.1 View Live Serial Telemetry (`/dev/ttyACM0`)
```bash
stty -F /dev/ttyACM0 115200 raw -echo && cat /dev/ttyACM0
```

### 3.2 Inspect Kernel Trace Buffer & Fault Struct via OpenOCD
```bash
openocd -f interface/stlink.cfg -f target/stm32l4x.cfg \
  -c "init; halt" \
  -c "echo [mdw 0x20000004 8]" \
  -c "echo [mdw 0x20000028 16]" \
  -c "echo [mdw 0x2000006c 24]" \
  -c "resume; exit"
```
