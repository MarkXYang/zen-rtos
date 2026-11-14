# ZenRTOS User Porting Guide

This guide explains how **ZenRTOS** is structured for portability and provides a step-by-step blueprint for porting it to any new microcontroller (MCU)—whether another **ARM Cortex-M3/M4/M7/M33** chip (such as STM32F4, STM32H7, nRF52840, RP2350), an **ARM Cortex-M0/M0+** chip (RP2040, STM32G0), or a **RISC-V** MCU (ESP32-C3, CH32V307, GD32VF103).

---

## 1. Architectural Boundaries: What Changes vs. What Stays Untouched

ZenRTOS is cleanly divided into four layers so that **90% of the kernel is 100% hardware-independent**:

| Target Chip Category | `kernel/` | `arch/` | `bsp/<chip>/` | Effort |
| :--- | :--- | :--- | :--- | :--- |
| **Same CPU Core** (Any ARM Cortex-M3 / M4 / M7 / M33: STM32F4, STM32H7, nRF52, GD32F4) | **No changes** | **No changes** | Create `link.ld`, `startup.s`, `<chip>.h` | **~30 mins** |
| **ARMv6-M Core** (ARM Cortex-M0 / M0+: RP2040, STM32F0/G0/L0) | **No changes** | Minor update in `context.s` (`R8–R11` via `mov`) | Create `link.ld`, `startup.s`, `<chip>.h` | **~1 hour** |
| **Different ISA** (e.g., RISC-V `RV32IMAC`: ESP32-C3, CH32V307) | Update stack frame in `rtos_task_create()` & timer CSRs | New `arch/riscv/rv32/context.s` | Create `link.ld`, `startup.s`, `<chip>.h` | **~2–4 hours** |

---

## 2. Porting Scenario A: New ARM Cortex-M3 / M4 / M7 / M33 MCU (Most Common)

Because the **SysTick (`0xE000E010`)**, **PendSV (`0xE000ED04`)**, and **SCB Fault Registers (`0xE000ED28`)** are standardized by ARM across all Cortex-M3, M4, M7, and M33 microcontrollers, `kernel/rtos.c` and `arch/arm/cortex-m4/context.s` **require zero modifications**.

You only need to create three files under `bsp/<your_chip>/`:

```text
bsp/<your_chip>/
├── link.ld       # Step 1: Flash & SRAM base addresses and sizes
├── startup.s     # Step 2: Interrupt vector table count & IRQ slot for UART
└── <your_chip>.h # Step 3: GPIO & UART register addresses + clock frequency
```

---

### Step 1: Create the Linker Script (`bsp/<chip>/link.ld`)

Open your new chip's datasheet ("Memory Map" section) and find:
1. **Flash Base Address & Size** (e.g., `0x08000000` on STM32/GD32, `0x00000000` on nRF52, `0x10000000` on RP2350).
2. **SRAM Base Address & Size** (e.g., `0x20000000` on almost all Cortex-M chips).

Copy `bsp/stm32l476/link.ld` to `bsp/<chip>/link.ld` and update only the `MEMORY` block:

```ld
/* Example: Porting to STM32F407VG (1024 KB Flash, 128 KB main SRAM) */
MEMORY
{
    FLASH (rx)  : ORIGIN = 0x08000000, LENGTH = 1024K
    RAM   (rwx) : ORIGIN = 0x20000000, LENGTH = 128K
}

/* _estack is automatically calculated as ORIGIN(RAM) + LENGTH(RAM) */
_estack = ORIGIN(RAM) + LENGTH(RAM);
```

Keep the `.isr_vector`, `.text`, `.data`, and `.bss` section definitions identical to `bsp/stm32l476/link.ld` because `Reset_Handler` relies on the linker symbols `_sidata`, `_sdata`, `_edata`, `_sbss`, `_ebss`, and `_estack`.

---

### Step 2: Adapt the Vector Table (`bsp/<chip>/startup.s`)

In `bsp/stm32l476/startup.s`, the **first 16 entries (Words 0–15)** of `g_pfnVectors` are standard across **every ARM Cortex-M chip**:
- Word `0`: `_estack`
- Word `1`: `Reset_Handler`
- Word `3`: `HardFault_Handler`
- Word `14`: `PendSV_Handler`
- Word `15`: `SysTick_Handler`

Only the **Peripheral IRQ entries (Word 16 onward)** differ by chip:
1. Look up the **IRQ number (`IRQn`)** of your UART peripheral in the chip's Reference Manual ("Vector Table" chapter).
   - For example, on `STM32L476` and `STM32F407`, `USART2_IRQn` is **IRQ 38**.
   - On `STM32F103`, `USART1_IRQn` is **IRQ 37**.
2. Adjust the `.rept` padding before and after your UART ISR in `startup.s`:

```armasm
    /* If your UART is IRQ #N (e.g., N = 38): */
    .rept 38
    .word Default_Handler
    .endr

    .word USART2_IRQHandler   /* Placed at Peripheral IRQ #38 */

    .rept 43                  /* Remaining IRQs up to chip's max IRQ count */
    .word Default_Handler
    .endr
```

---

### Step 3: Implement the Minimal BSP Header (`bsp/<chip>/<chip>.h`)

ZenRTOS only requires 6 hardware helper functions in your BSP header to run the LED task, debug telemetry, and interactive `zensh>` shell. Copy `bsp/stm32l476/stm32l476.h` and update the peripheral register addresses in these 6 functions:

| Required BSP Function | What to Adapt for Your New Chip |
| :--- | :--- |
| `bsp_init_led(void)` | Enable GPIO port clock in RCC and set LED pin mode to Output. |
| `bsp_toggle_led(void)` | XOR the LED pin bit in the GPIO Output Data Register (`ODR`). |
| `bsp_set_led(bool on)` | Set or clear the LED pin bit via `BSRR` / `ODR`. |
| `bsp_get_led(void)` | Read the current LED pin bit from `ODR`. |
| `bsp_init_uart(cpu_hz, baud)` | Enable UART + GPIO clocks, set TX/RX Alternate Function pins, set baud rate divider (`cpu_hz / baud`), enable RX interrupt (`RXNEIE`), and enable the IRQ bit in `NVIC_ISER`. |
| `bsp_uart_putc(char c)` & `USARTx_IRQHandler()` | Wait for TX Empty flag (`TXE`), write byte to TX register (`TDR`/`DR`); in ISR, read byte from RX register (`RDR`/`DR`) and push to `g_uart_rx_buf`. |

All shell parsing (`bsp_shell_poll`, `bsp_shell_exec`), number formatting (`bsp_uart_putdec`, `bsp_uart_puthex`), and task table printing (`bsp_debug_dump_status`) only call `bsp_uart_putc()` and `bsp_uart_getc()`, so they work on your new chip **without changing a single line**.

---

### Step 4: Set Your CPU Clock Frequency in `app/main.zc` & `Makefile`

1. **In `app/main.zc`**:
   Pass your new chip's default boot clock frequency (in Hz) to `bsp_init_uart()` and `rtos_start()`:
   - **STM32L476** boots on a **4 MHz** MSI oscillator (`4000000`).
   - **STM32F407 / STM32F411** boots on a **16 MHz** HSI oscillator (`16000000`).
   - **STM32F103** boots on an **8 MHz** HSI oscillator (`8000000`).
   - **nRF52840 / STM32H7** boots on a **64 MHz** oscillator (`64000000`).

   ```zc
   def CPU_FREQ_HZ = 16000000; // Example: 16 MHz HSI on STM32F4

   fn main() -> int {
       bsp::bsp_init_uart(CPU_FREQ_HZ, 115200);
       // ...
       rtos::rtos_start(CPU_FREQ_HZ, 1000); // 1000 Hz (1 ms) SysTick
       return 0;
   }
   ```

2. **In `Makefile`**:
   Update the include path `-Ibsp/<your_chip>`, linker script `-Wl,-T,bsp/<your_chip>/link.ld`, and OpenOCD target config (e.g., `-f target/stm32f4x.cfg`).

---

## 3. Porting Scenario B: ARM Cortex-M0 / M0+ (ARMv6-M)

Chips like the **RP2040**, **STM32F0**, **STM32G0**, and **STM32L0** use the **ARMv6-M** instruction set, which has one assembly restriction compared to Cortex-M3/M4:
- Thumb-1 `stmia` / `ldmia` instructions on Cortex-M0/M0+ can only directly access **low registers (`R0–R7`)**, not high registers (`R8–R11`), and `stmdb r0!, {r4-r11}` is not supported.

To port `arch/arm/cortex-m4/context.s` to `arch/arm/cortex-m0/context.s`:

```armasm
PendSV_Handler:
    cpsid i
    mrs   r0, psp
    subs  r0, r0, #32       /* Allocate 8 words (32 bytes) on PSP for R4-R11 */

    /* 1. Save low registers R4-R7 into [r0 .. r0+12] */
    stmia r0!, {r4-r7}
    /* 2. Move high registers R8-R11 into R4-R7, then save into [r0+16 .. r0+28] */
    mov   r4, r8
    mov   r5, r9
    mov   r6, r10
    mov   r7, r11
    stmia r0!, {r4-r7}
    subs  r0, r0, #32       /* Rewind r0 to bottom of saved R4-R11 frame */

    /* 3. Save outgoing PSP and swap current_tcb = next_tcb */
    ldr   r1, =current_tcb
    ldr   r2, [r1]
    str   r0, [r2]
    ldr   r3, =next_tcb
    ldr   r4, [r3]
    str   r4, [r1]

    /* 4. Restore incoming R4-R11 from next_tcb->sp */
    ldr   r0, [r4]
    adds  r0, r0, #16
    ldmia r0!, {r4-r7}      /* Pop saved R8-R11 into R4-R7 */
    mov   r8, r4
    mov   r9, r5
    mov   r10, r6
    mov   r11, r7
    msr   psp, r0           /* r0 is now at top of software frame (bottom of HW frame) */
    subs  r0, r0, #32
    ldmia r0!, {r4-r7}      /* Pop saved R4-R7 */

    cpsie i
    ldr   r0, =0xFFFFFFFD
    bx    r0
```

---

## 4. Porting Scenario C: Non-ARM Architecture (e.g., RISC-V `RV32IMAC`)

To port ZenRTOS to a **32-bit RISC-V MCU** (such as ESP32-C3, GD32VF103, or WCH CH32V307), only 3 kernel hooks change:

1. **Critical Sections (`rtos_enter_critical` / `rtos_exit_critical`)**:
   Replace `cpsid i` / `cpsie i` with clearing/setting the `MIE` bit (Bit 3) in the `mstatus` CSR:
   ```c
   static inline void rtos_enter_critical(void) {
       __asm volatile ("csrci mstatus, 0x8" : : : "memory");
   }
   static inline void rtos_exit_critical(void) {
       __asm volatile ("csrsi mstatus, 0x8" : : : "memory");
   }
   ```
2. **Initial Stack Frame (`rtos_task_create`)**:
   Instead of the 16-word ARM Cortex-M frame, allocate 32 words on the stack for RISC-V registers `x1 (ra)` through `x31 (t6)` plus `mepc` (set to `func`) and `mstatus` (set to `0x1880` for `MPP = Machine Mode, MPIE = 1`), with `x10 (a0) = (uint32_t)arg` and `x1 (ra) = (uint32_t)rtos_task_exit_hook`.
3. **Timer & Software Interrupts**:
   - Configure the RISC-V Core-Local Interruptor (**CLINT** / **MTIME** & **MTIMECMP**) to fire the 1000 Hz tick ISR.
   - Trigger the **Machine Software Interrupt (`MSIP`)** at lowest priority as the equivalent of `PendSV_Handler`.

---

## 5. Step-by-Step Verification Checklist for a New Port

When bringing up ZenRTOS on a brand-new board, verify your port in these 5 incremental stages:

- [ ] **Stage 1 — Host Unit Tests**: Run `make test` on your host machine to confirm all 9 kernel unit tests pass before touching hardware.
- [ ] **Stage 2 — Linker & Reset Handler Verification**: Flash the binary, halt via OpenOCD/GDB (`init; halt; reg`), and verify that `sp` (`MSP`) is inside your chip's valid SRAM range (`0x2000xxxx`) and `pc` reaches `main()`.
- [ ] **Stage 3 — First Task Launch (`PSP` Transition)**: Let the board run for 100 ms, halt with OpenOCD, and check that `Current mode: Thread` uses **`psp`** (not `msp`) and `current_tcb` points to `&g_tasks[0]` (or your highest-priority ready task).
- [ ] **Stage 4 — `SysTick` 1000 Hz Timing Accuracy**: Halt the target, read `g_system_ticks`, resume for `1000 ms` (`sleep 1000`), halt again, and verify `g_system_ticks` increased by `~1000` ($\pm 5$ ticks). If it increased by `4000` or `250`, adjust `cpu_freq_hz` passed to `rtos_start()`.
- [ ] **Stage 5 — Interactive `zensh>` Shell**: Connect to the serial port (`115200 8N1`) and run `ps`, `mem`, and `trace` to confirm `Stack Canary Check (0xDEADBEEF): PASS` and clean task switching.
