# Step-by-Step Porting Plan: ZenRTOS on NXP i.MX RT1021 (ARM Cortex-M7)

This document provides the architectural outline and phased engineering plan for adding **NXP i.MX RT1021 (`MIMXRT1021`, ARM Cortex-M7 @ up to 500 MHz)** support to **ZenRTOS**, alongside the existing **STM32L476RG (ARM Cortex-M4)** and **ESP32-C6-Pico (RISC-V `RV32IMAC`)** ports.

---

## 1. Architectural Comparison Across ZenRTOS Targets

| Subsystem | STM32L476RG (Existing) | ESP32-C6-Pico (Existing) | NXP i.MX RT1021 (New Port) |
| :--- | :--- | :--- | :--- |
| **CPU Core & ISA** | ARM Cortex-M4 (`Thumb-2`) @ 4 MHz MSI | RISC-V (`RV32IMAC`) @ 160 MHz | ARM Cortex-M7 (`Thumb-2`) @ 24 MHz XTAL / 500 MHz PLL6 |
| **Compiler Target** | `clang --target=arm-none-eabi -mcpu=cortex-m4` | `clang --target=riscv32-unknown-elf -march=rv32imac` | `clang --target=arm-none-eabi -mcpu=cortex-m7` |
| **Internal RAM** | `96 KB` SRAM1 (`0x20000000`) + `32 KB` SRAM2 | `512 KB` HP SRAM (`0x40800000`) | `256 KB` FlexRAM: `64 KB` ITCM (`0x00000000`), `64 KB` DTCM (`0x20000000`), `128 KB` OCRAM (`0x20200000`) |
| **Flash / Code Storage** | `1 MB` Internal Flash (`0x08000000`) | External SPI Flash / Direct-Boot SRAM | **Flashless crossover MCU**: External QSPI NOR XIP via FlexSPI (`0x60000000`) or RAM boot |
| **Boot Header** | Raw Cortex-M vector table at `0x08000000` | 8-byte Direct Boot magic (`0xAEDB041D`) | **FCB** at `0x60000400` + **IVT & BootData** at `0x60001000` -> Vector Table at `0x60002000` |
| **Boot Watchdogs** | Disabled at reset | Enabled by ROM (`TIMG0`, `LP_WDT`, `SWD`) | **`RTWDOG` (`WDOG3`) / `WDOG1` active/configurable at reset**; unlock & disable in early init |
| **Stack Pointers** | Dual (`MSP` for ISRs, `PSP` for tasks) | Single architectural `sp` (`x2`) | Dual (`MSP` in DTCM for ISRs, `PSP` in DTCM for tasks) |
| **Context Switcher** | `PendSV_Handler` (`SCB_ICSR` bit 28) | `CLINT_MSIP` (`0x20001800`, Slot 3) | `PendSV_Handler` (`SCB_ICSR` bit 28) + Cortex-M7 `dsb`/`isb` barriers |
| **1000 Hz Tick Timer** | `SysTick_Handler` (`0xE000E010`) | `CLINT_MTIME` / `MTIMECMP` (Slot 7) | `SysTick_Handler` (`0xE000E010`, `SYST_RVR = 499,999` @ 500 MHz) |
| **Serial Console** | `USART2` (`PA2`/`PA3`, IRQ 38) | Built-in USB-Serial/JTAG (`0x6000F000`) | `LPUART1` (`0x40184000`, `GPIO_AD_B0_06`/`07` `ALT2`, IRQ 20) |
| **User LED** | `PA5` (`GPIOA` bit 5) | `GPIO8` (`WS2812B` NeoPixel) | `GPIO_AD_B0_05` (`ALT5` -> `GPIO1_IO05` on `MIMXRT1020-EVK`) |

---

## 2. Layer-by-Layer Impact Matrix

| Layer | Files | Planned Changes |
| :--- | :--- | :--- |
| **Kernel (`kernel/`)** | `kernel/include/rtos.h`, `kernel/rtos.c` | **No changes required.** Standard ARMv7-M `PendSV` (`0xE000ED04`), `SysTick` (`0xE000E010`), `SCB_SHPR3` (`0xE000ED20`), and Fault Status registers (`CFSR`, `HFSR`, `MMFAR`, `BFAR`) are identical on Cortex-M7. At 500 MHz and 1000 Hz tick, `SYST_RVR = 499,999` (`0x7A11F`), well within the 24-bit `SYST_RVR` limit (`0xFFFFFF`). |
| **Arch (`arch/`)** | `arch/arm/cortex-m7/context.s` | Add a clean Cortex-M7 assembly port (`.cpu cortex-m7`, `-mfloat-abi=soft`) with explicit `dsb`/`isb` pipeline synchronization barriers required by the superscalar Cortex-M7 pipeline, leaving `cortex-m4/context.s` untouched. |
| **App (`app/`)** | `app/main.zc`, `bsp/stm32l476/stm32l476.zc`, `bsp/esp32c6/esp32c6.zc` | Replace the hardcoded `4000000` (4 MHz STM32 MSI clock) in `app/main.zc` with `extern fn bsp_get_cpu_freq_hz() -> u32;` so `bsp_init_uart()` and `rtos_start()` automatically receive each board's true core frequency. |
| **BSP (`bsp/rt1021/`)** | `link_ram.ld`, `link_xip.ld`, `boot_hdr.c`, `startup.s`, `rt1021.zc`, `openocd.cfg` | New NXP i.MX RT1021 Board Support Package in freestanding Zen-C, supporting both fast RAM bring-up and cold-boot FlexSPI NOR XIP execution. |
| **Build (`Makefile`)** | `Makefile` | Add `rt1021` Zen-C transpilation, ELF/BIN build rules (`RT1021_BOOT=xip|ram`), `flash-rt1021`, and `shell-rt1021` targets without affecting existing targets. |

---

## 3. Target Repository Layout

```text
zen-rtos/
├── arch/
│   ├── arm/
│   │   ├── cortex-m4/context.s      # Unchanged (STM32L476)
│   │   └── cortex-m7/context.s      # [NEW] Cortex-M7 PendSV, first-task launch & fault entry
│   └── riscv/rv32/context.s         # Unchanged (ESP32-C6)
├── bsp/
│   ├── stm32l476/stm32l476.zc       # [UPDATE] Implement bsp_get_cpu_freq_hz() -> 4000000
│   ├── esp32c6/esp32c6.zc           # [UPDATE] Implement bsp_get_cpu_freq_hz() -> 160000000
│   └── rt1021/                      # [NEW] NXP i.MX RT1021 BSP
│       ├── link_ram.ld              # [NEW] ITCM/OCRAM + DTCM linker script (GDB/OpenOCD RAM load)
│       ├── link_xip.ld              # [NEW] FlexSPI NOR XIP (0x60000000) + DTCM linker script
│       ├── boot_hdr.c               # [NEW] FlexSPI FCB (@ +0x400) + IVT & BootData (@ +0x1000)
│       ├── startup.s                # [NEW] Cortex-M7 vector table, VTOR setup, early init, .data/.bss
│       ├── rt1021.zc                # [NEW] WDOG, CCM, IOMUXC, GPIO1 LED, LPUART1 ISR & zensh> shell
│       └── openocd.cfg              # [NEW] CMSIS-DAP / J-Link OpenOCD configuration
├── app/
│   └── main.zc                      # [UPDATE] Call bsp_get_cpu_freq_hz() instead of literal 4000000
├── docs/
│   └── PORTING_PLAN_NXP_RT1021.md   # [THIS DOCUMENT]
└── Makefile                         # [UPDATE] Add rt1021, flash-rt1021, shell-rt1021 targets
```

---

## 4. Key NXP i.MX RT1021 Hardware Requirements

### 4.1 Crossover Memory Map & Dual Linker Strategy
Unlike traditional microcontrollers with internal flash at `0x08000000`, the `MIMXRT1021` provides:
- **ITCM (`0x00000000`, 64 KB default)**: Zero-wait-state instruction tightly coupled memory.
  - *Important*: Address `0x00000000` is physically mapped to ITCM byte 0. Reserve the bottom `0x400` bytes in `link_ram.ld` so valid code/data addresses never collide with `NULL` (`0x0`).
- **DTCM (`0x20000000`, 64 KB default)**: Zero-wait-state data tightly coupled memory. Place `.data`, `.bss`, task stacks, TCBs, and `_estack` (`0x20010000`) here for deterministic single-cycle context switches.
- **OCRAM (`0x20200000`, 128 KB default)**: General-purpose on-chip system SRAM (ideal for holding `.text`/`.rodata` during initial GDB/OpenOCD RAM-load bring-up).
- **FlexSPI1 AMBA XIP (`0x60000000`, 8 MB IS25LP064A on `MIMXRT1020-EVK`)**:
  - **Offset `0x400` (`0x60000400`)**: 512-byte **FlexSPI Configuration Block (FCB)** with magic `"FCFB"` (`0x42464346`). *(Note: RT1021 places FCB at `+0x400`, unlike RT1050/RT1060 which place it at `+0x000`.)*
  - **Offset `0x1000` (`0x60001000`)**: 32-byte **Image Vector Table (IVT)** (`header = 0x402000D1`, `entry = &Reset_Handler`, `boot_data = &g_boot_data`) followed by **BootData** (`start = 0x60000000`, `size = 8M`, `plugin = 0`) at `0x60001020`.
  - **Offset `0x2000` (`0x60002000`)**: Cortex-M7 `.isr_vector` (`g_pfnVectors`, 1024-byte aligned for `SCB->VTOR`) followed by `.text` and `.rodata`.

### 4.2 Early Watchdog Disable & `SCB->VTOR` Setup
Immediately inside `Reset_Handler` (before zeroing `.bss` or copying `.data`), `bsp_early_init()` must:
1. Write `SCB_VTOR` (`0xE000ED08`) = `(uintptr_t)&g_pfnVectors` followed by `dsb` + `isb`, ensuring `PendSV`, `SysTick`, `HardFault`, and `LPUART1` dispatch to our vector table rather than the NXP Boot ROM table.
2. Disable **`RTWDOG` (`WDOG3`, base `0x400BC000`)**:
   - Write unlock key `0xD928C520` to `RTWDOG_CNT` (`0x400BC004`), wait until `RTWDOG_CS[ULK]` (bit 11) asserts, set `RTWDOG_TOVAL = 0xFFFF` (`0x400BC008`), and clear `EN` (bit 7) while setting `UPDATE` (bit 5) in `RTWDOG_CS` (`0x400BC000`).
3. Disable **`WDOG1` (base `0x400B8000`)** if active (`WDOG1_WCR` 16-bit clear of `WDE` bit 2).

### 4.3 Clock Tree (`CCM` & `CCM_ANALOG`)
- **Base `CCM`**: `0x400FC000` | **Base `CCM_ANALOG`**: `0x400D8000`
- **Core Clock (`ahb_clk_root`)**:
  - *Stage A (Initial Bring-Up)*: Run directly from the **24 MHz crystal oscillator (`osc_clk`)** (`bsp_get_cpu_freq_hz() = 24000000`) for zero-PLL-risk initial verification.
  - *Stage B (Full Speed)*: Enable **System/ENET PLL (`PLL6`, 500 MHz)** in `CCM_ANALOG_PLL_ENET` and switch `periph_clk` -> `ahb_clk_root` (`AHB_PODF = 0`, `IPG_PODF = 3` -> **500 MHz Core / 125 MHz IPG**; `bsp_get_cpu_freq_hz() = 500000000`).
- **UART Clock (`uart_clk_root`)**:
  - Set `CCM_CSCDR1[UART_CLK_SEL] = 1` and `CCM_CSCDR1[UART_CLK_PODF] = 0` so `LPUART1` always runs from a clean, fixed **24 MHz** reference (`osc_clk`) independent of core PLL scaling.
  - Ungate `LPUART1` clock in `CCM_CCGR5` (`CG12`, bits `25:24 = 0b11`) and `IOMUXC` / `GPIO1` clocks in `CCM_CCGR1` / `CCM_CCGR4`.

### 4.4 Pin Multiplexing (`IOMUXC`), User LED (`GPIO1`), and Console (`LPUART1`)
Using exact **MIMXRT1021** (93-pad `IOMUXC` @ `0x401F8000`) register addresses for **MIMXRT1020-EVK**:

| Function | MCU Pad | Mux Register (`ALT`) | Pad Control Register | Peripheral Base & Details |
| :--- | :--- | :--- | :--- | :--- |
| **User LED** | `GPIO_AD_B0_05` | `0x401F80D0` = `0x5` (`ALT5`) | `0x401F8244` = `0x10B0` | `GPIO1` (`0x401B8000`), pin 5 (`DR` `+0x00`, `GDIR` `+0x04`, `DR_SET` `+0x84`, `DR_CLEAR` `+0x88`, `DR_TOGGLE` `+0x8C`) |
| **LPUART1 TX** | `GPIO_AD_B0_06` | `0x401F80D4` = `0x2` (`ALT2`) | `0x401F8248` = `0x10B0` | `LPUART1` (`0x40184000`), `BAUD` `+0x10`, `STAT` `+0x14`, `CTRL` `+0x18`, `DATA` `+0x1C` |
| **LPUART1 RX** | `GPIO_AD_B0_07` | `0x401F80D8` = `0x2` (`ALT2`) | `0x401F824C` = `0x10B0` | `LPUART1_IRQn = 20` (`NVIC_ISER0` `0xE000E100` bit 20; vector slot `16 + 20 = 36`) |

- **Exact 115200 Baud Calculation** (from 24 MHz `uart_clk_root`):
  - Set `OSR = 15` (16x oversampling) and `SBR = 13` in `LPUART1_BAUD`: `24,000,000 / (16 * 13) = 115,384.6 bps` (**0.16% baud error**).

---

## 5. Phased Implementation Plan

### Phase 1: Board-Agnostic CPU Frequency Hook
1. Update `app/main.zc` to declare `extern fn bsp_get_cpu_freq_hz() -> u32;` and pass `cpu_hz` to `bsp_init_uart(cpu_hz, 115200)` and `rtos::rtos_start(cpu_hz, 1000)`.
2. Add `fn bsp_get_cpu_freq_hz() -> u32` returning `4000000` in `bsp/stm32l476/stm32l476.zc` and `160000000` in `bsp/esp32c6/esp32c6.zc`.
3. Run `make test`, `make build`, and `make esp32c6` to confirm zero regressions on existing ports.

### Phase 2: Cortex-M7 Context Switcher (`arch/arm/cortex-m7/context.s`)
1. Create `arch/arm/cortex-m7/context.s` based on `arch/arm/cortex-m4/context.s` with `.cpu cortex-m7`.
2. Add `dsb` and `isb` synchronization barriers around `msr psp, r0` and `msr control, r1` per ARM Cortex-M7 architectural guidelines (ARM DAI 0321A).
3. Keep `HardFault_Handler`, `MemManage_Handler`, `BusFault_Handler`, and `UsageFault_Handler` forwarding the active `msp`/`psp` frame pointer to `rtos_fault_capture()`.

### Phase 3: RT1021 Linker Scripts, FlexSPI XIP Headers & Startup Assembly
1. **`bsp/rt1021/link_ram.ld`**:
   - Place `.isr_vector`, `.text`, and `.rodata` in `OCRAM` (`0x20200000`, 128 KB) and `.data`, `.bss`, and `_estack` in `DTCM` (`0x20000000`, 64 KB) for instant GDB/OpenOCD RAM execution without touching external QSPI flash.
2. **`bsp/rt1021/link_xip.ld` & `bsp/rt1021/boot_hdr.c`**:
   - Place `.boot_hdr.conf` (FCB) at `0x60000400`, `.boot_hdr.ivt` + `.boot_hdr.boot_data` at `0x60001000`, `.isr_vector` (aligned to `0x400`) + `.text` + `.rodata` at `0x60002000` (`AT > FLASH`), and `.data` / `.bss` / `_estack` in `DTCM` (`0x20000000`).
3. **`bsp/rt1021/startup.s`**:
   - Define `g_pfnVectors` (16 Cortex-M7 core exceptions + 160 RT1021 peripheral IRQs, placing `LPUART1_IRQHandler` at peripheral IRQ `#20` / word `#36`).
   - In `Reset_Handler`: load `sp = _estack`, write `SCB->VTOR = g_pfnVectors`, call `bsp_early_init` (watchdogs + clocks), copy `.data` from Flash to DTCM, zero `.bss` in DTCM, and `bl main`.

### Phase 4: Zen-C RT1021 BSP (`bsp/rt1021/rt1021.zc`)
1. Implement `@used fn bsp_early_init()` to unlock/disable `RTWDOG` and `WDOG1`, configure CCM clocks (24 MHz `uart_clk_root` + configurable 24 MHz XTAL / 500 MHz `PLL6` core clock), and optionally enable Cortex-M7 L1 I-Cache (`SCB_ICIALLU` + `SCB_CCR[IC]`).
2. Implement `bsp_get_cpu_freq_hz()`, `bsp_init_led()`, `bsp_toggle_led()`, `bsp_set_led()`, and `bsp_get_led()` using `GPIO1_DR_SET` / `DR_CLEAR` / `DR_TOGGLE`.
3. Implement `bsp_init_uart()`, `bsp_uart_putc()`, `bsp_uart_getc()`, and `@used fn LPUART1_IRQHandler()` (clearing overrun `OR` flag if set, draining `DATA` while `STAT[RDRF]` is set into the 128-byte RX ring buffer).
4. Implement the `zensh>` interactive shell (`help`, `ps`, `trace`, `sensor`, `mem`, `led on|off|toggle|blink`, `log on|off`, `fault`, `uptime`, `clear`, and `reboot` via `SCB_AIRCR = 0x05FA0004`).

### Phase 5: Makefile & Tooling Integration
1. Add `RT1021_BOOT ?= xip` selector (`link_xip.ld` + `boot_hdr.c` when `xip`, `link_ram.ld` when `ram`).
2. Add `$(BUILD_DIR)/rt1021_zc.c` rule to `make zenc`.
3. Add `make rt1021` producing `build/zen_rtos_rt1021.elf` and `build/zen_rtos_rt1021.bin` using `clang --target=arm-none-eabi -mcpu=cortex-m7 -mthumb -mfloat-abi=soft` and `-fuse-ld=lld`.
4. Add `flash-rt1021` (OpenOCD / pyOCD) and `shell-rt1021` (`miniterm` 115200 8N1) convenience targets.

### Phase 6: Verification & Bring-Up Checklist
- [ ] **Step 1 — Host & Cross-Build Sanity**:
  - `make test` passes all Linux host scheduler tests.
  - `make build` (STM32L476), `make esp32c6`, `make rt1021 RT1021_BOOT=ram`, and `make rt1021 RT1021_BOOT=xip` all compile and link cleanly with zero errors.
  - Verify ELF layout with `llvm-readelf -S` / `llvm-objdump -h`: confirm FCB at `0x60000400`, IVT at `0x60001000`, `.isr_vector` at `0x60002000`, and `.data`/`.bss` in `0x2000xxxx`.
- [ ] **Step 2 — Bare-Metal RAM Boot & Watchdog Survival (`RT1021_BOOT=ram`)**:
  - Load `build/zen_rtos_rt1021.elf` into OCRAM/DTCM over SWD, resume execution, and confirm the chip runs continuously past 5 seconds without a `WDOG1` or `RTWDOG` reset.
- [ ] **Step 3 — `LPUART1` Console & `1000 Hz` `SysTick` Timing**:
  - Connect serial terminal (`115200 8N1`), verify `[ZenRTOS] Booting Zen-C RTOS Application...` banner, and run `uptime` twice 10 seconds apart to confirm `g_system_ticks` increments by `~10,000` ticks (`1.000 ms` per tick).
- [ ] **Step 4 — Preemptive Multi-Tasking, LED & `zensh>` Shell**:
  - Verify 1 Hz LED blink on `GPIO1_IO05`, `ps`/`top` task runtime stats, `mem` (`Stack Canary Check (0xDEADBEEF): PASS`), `sensor` IPC queue counts, `trace` events, and `LPUART1_IRQHandler` interactive input.
- [ ] **Step 5 — FlexSPI NOR XIP Cold Boot (`RT1021_BOOT=xip`)**:
  - Program `build/zen_rtos_rt1021.elf` to external QSPI NOR at `0x60000000`, power-cycle the board with SWD disconnected, and verify standalone cold boot into `zensh>`.

---

## 6. Board-Specific Parameters to Confirm Before Coding

1. **Board Variant**: Confirm whether targeting the official **NXP MIMXRT1020-EVK** (`GPIO_AD_B0_05` LED, `GPIO_AD_B0_06`/`07` `LPUART1`, 8 MB `IS25LP064A` QSPI NOR) or a custom RT1021 board.
2. **Boot Mode**: Confirm supporting **both** `RT1021_BOOT=xip` (default) and `RT1021_BOOT=ram`, or only one.
3. **Debug / Flash Probe**: Confirm preferred flashing tool for `make flash-rt1021` (OpenSDA **CMSIS-DAP** via OpenOCD/pyOCD, **SEGGER J-Link**, or NXP `blhost`).
