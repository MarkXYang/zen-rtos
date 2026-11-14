# Step-by-Step Porting Plan: ZenRTOS on ESP32-C6-Pico (RISC-V `RV32IMAC`)

This document provides a complete, self-contained engineering blueprint and code templates so you can port **ZenRTOS** step-by-step from the **STM32L476RG (ARM Cortex-M4)** to the **Waveshare ESP32-C6-Pico (Espressif ESP32-C6, 32-bit RISC-V `RV32IMAC`)**.

---

## 1. Architectural Comparison: STM32L476 vs. ESP32-C6-Pico

| Subsystem | STM32L476RG (Current) | ESP32-C6-Pico (Target Port) |
| :--- | :--- | :--- |
| **CPU Core & ISA** | ARM Cortex-M4 (`Thumb-2`) | Espressif HP RISC-V (`RV32IMAC_zicsr_zifencei`) @ 160 MHz |
| **Compiler Target** | `clang --target=arm-none-eabi -mcpu=cortex-m4` | `clang --target=riscv32-unknown-elf -march=rv32imac_zicsr_zifencei -mabi=ilp32` |
| **Main SRAM** | `96 KB` at `0x20000000` | `512 KB` Unified HP SRAM at `0x40800000` (`0x40800000 .. 0x4087FFFF`) |
| **Flash Execution** | XIP at `0x08000000` | Cache-mapped XIP at `0x42000000` (with 8-byte Direct Boot header `0xAEDB041D`) |
| **Boot Watchdogs** | Disabled by default at reset | **Enabled by ROM at reset!** Must unlock & disable `TIMG0` WDT, `RWDT`, and `SWD` in `startup.s` / `bsp_init` |
| **Stack Pointers** | Dual (`MSP` for ISRs, `PSP` for tasks) | Single architectural `sp` (`x2`) |
| **Context Switcher** | `PendSV_Handler` (`SCB_ICSR` bit 28) | RISC-V Machine Software Interrupt (`MSIP` at `0x20001C00`, CLINT IRQ #3) |
| **1000 Hz Tick Timer** | `SysTick_Handler` (`0xE000E010`) | RISC-V Machine Timer (`MTIME` `0x20001800` & `MTIMECMP` `0x20001808`, CLINT IRQ #7) |
| **Serial Console** | `USART2` (`PA2`/`PA3` -> ST-LINK VCP) | Built-in **USB-Serial/JTAG Controller** (`0x6000F000`) over the onboard USB-C port (`/dev/ttyACM0`) |

---

## 2. Target File Structure to Add

When porting to `esp32c6`, you will add two new folders (`arch/riscv/rv32/` and `bsp/esp32c6/`) and guard the architecture-specific lines in `kernel/rtos.c` with `#ifdef RTOS_ARCH_RISCV`:

```text
zen-rtos/
├── arch/
│   ├── arm/cortex-m4/context.s      # Existing ARM Cortex-M4 context switcher
│   └── riscv/rv32/context.s         # [NEW] Step 3: RISC-V trap vector & context switcher
├── bsp/
│   ├── stm32l476/                   # Existing STM32L476 BSP
│   └── esp32c6/                     # [NEW] ESP32-C6-Pico BSP
│       ├── link.ld                  # [NEW] Step 1: Direct-Boot Flash & 512KB SRAM linker script
│       ├── startup.s                # [NEW] Step 2: Entry point, WDT disable, .data/.bss init
│       └── esp32c6.h                # [NEW] Step 5: USB-Serial/JTAG console, GPIO, WDT macros
├── kernel/
│   ├── include/rtos.h               # Unchanged! (sp is still at offset 0 of tcb_t)
│   └── rtos.c                       # [UPDATE] Step 4: Add #ifdef RTOS_ARCH_RISCV stack frame & CLINT timer
└── Makefile                         # [UPDATE] Step 6: Add esp32c6 build rule
```

---

## 3. Step-by-Step Implementation Guide

### Step 1: Create the Linker Script (`bsp/esp32c6/link.ld`)

The ESP32-C6 ROM bootloader supports **Direct Boot Mode** from Flash or direct loading into HP SRAM via `esptool.py load_ram`. When the first 8 bytes of the image are `0x1D 0x04 0xDB 0xAE 0x1D 0x04 0xDB 0xAE` (two little-endian words of `0xAEDB041D`), the ROM bootloader jumps directly to byte offset `+0x08` (`_start`) without requiring a second-stage bootloader.

Create `bsp/esp32c6/link.ld`:

```ld
ENTRY(_start)

MEMORY
{
    /* ESP32-C6 512 KB High-Performance SRAM (Unified Instruction + Data Bus) */
    SRAM (rwx) : ORIGIN = 0x40800000, LENGTH = 512K
}

/* Reserve top of 512 KB SRAM (below ROM stack area) for initial stack */
_estack = 0x4087C000;

SECTIONS
{
    .text :
    {
        . = ALIGN(4);
        KEEP(*(.direct_boot_header)) /* 8-byte magic: 0xAEDB041D, 0xAEDB041D */
        KEEP(*(.init))               /* _start entry point at offset +0x08   */
        . = ALIGN(256);
        KEEP(*(.mtvec_table))        /* 256-byte aligned RISC-V trap table   */
        *(.text)
        *(.text*)
        *(.rodata)
        *(.rodata*)
        . = ALIGN(4);
        _etext = .;
    } > SRAM

    _sidata = LOADADDR(.data);
    .data :
    {
        . = ALIGN(4);
        _sdata = .;
        *(.data)
        *(.data*)
        *(.sdata)
        *(.sdata*)
        . = ALIGN(4);
        _edata = .;
    } > SRAM

    .bss :
    {
        . = ALIGN(4);
        _sbss = .;
        *(.bss)
        *(.bss*)
        *(.sbss)
        *(.sbss*)
        *(COMMON)
        . = ALIGN(4);
        _ebss = .;
    } > SRAM
}
```

---

### Step 2: Create the Startup & Watchdog Disable Assembly (`bsp/esp32c6/startup.s`)

**Critical ESP32-C6 Hardware Requirement**: On reset, the ESP32-C6 ROM enables three hardware watchdog timers (`TIMG0 WDT`, `LP/RTC WDT`, and `Super WDT`). Your startup code must unlock and disable them immediately, otherwise the chip will reset itself every ~1.6 seconds.

Create `bsp/esp32c6/startup.s`:

```armasm
.section .direct_boot_header, "a"
.global _direct_boot_magic
_direct_boot_magic:
    .word 0xAEDB041D
    .word 0xAEDB041D

.section .init, "ax"
.global _start
.type _start, @function

_start:
    /* 1. Disable global machine interrupts (mstatus.MIE = 0) */
    csrci mstatus, 0x8

    /* 2. Initialize Stack Pointer (sp) to _estack */
    la    sp, _estack

    /* 3. Point mtvec to our 256-byte-aligned RISC-V Vectored Trap Table (mode = 1) */
    la    t0, _mtvec_table
    ori   t0, t0, 1
    csrw  mtvec, t0

    /* 4. Zero-initialize .bss (_sbss .. _ebss) */
    la    t0, _sbss
    la    t1, _ebss
1:
    bgeu  t0, t1, 2f
    sw    zero, 0(t0)
    addi  t0, t0, 4
    j     1b
2:
    /* 5. Disable ESP32-C6 hardware watchdog timers in C helper, then call main() */
    call  bsp_disable_watchdogs
    call  main

3:
    j     3b
```

---

### Step 3: Create the RISC-V Context Switcher & Trap Vector (`arch/riscv/rv32/context.s`)

On RISC-V (`RV32IMAC`), there is no hardware auto-stacking of registers on interrupt entry. Instead, our unified trap/context handler saves the 32-word task context frame (`x1..x31` + `mepc` + `mstatus` = 128 bytes) directly onto the outgoing task's stack, updates `current_tcb->sp = sp` (at offset `0` of `tcb_t`), switches `current_tcb = next_tcb`, loads `sp = next_tcb->sp`, restores `mepc`, `mstatus`, and `x1..x31`, and executes `mret`.

#### RISC-V 32-Word (128-Byte) Task Stack Frame Layout
| Word Offset | Byte Offset | Register | Description |
| :--- | :--- | :--- | :--- |
| `sp[0]` | `0` | `mepc` | Task Program Counter (`PC` to resume on `mret`) |
| `sp[1]` | `4` | `mstatus` | Machine Status (`0x1880`: `MPP=3` Machine Mode, `MPIE=1`) |
| `sp[2]` | `8` | `x1 (ra)` | Return Address (`rtos_task_exit_hook` on creation) |
| `sp[3..8]` | `12..32` | `x5..x7, x28..x30` | Caller-saved temporaries (`t0..t5`) |
| `sp[9]` | `36` | `x10 (a0)` | First function argument (`void *arg` on creation) |
| `sp[10..16]` | `40..64` | `x11..x17` | Function arguments (`a1..a7`) |
| `sp[17..28]` | `68..112` | `x8..x9, x18..x27` | Callee-saved registers (`s0/fp, s1, s2..s11`) |
| `sp[29]` | `116` | `x31 (t6)` | Temporary `t6` |

Create `arch/riscv/rv32/context.s`:

```armasm
.section .mtvec_table, "ax"
.balign 256
.global _mtvec_table
_mtvec_table:
    j riscv_exception_handler     /* Vectored Slot 0: Synchronous Exceptions */
    .rept 2
    j Default_IRQ_Handler         /* Slots 1..2 */
    .endr
    j riscv_msip_handler          /* Slot 3: CLINT Machine Software Interrupt (PendSV equivalent) */
    .rept 3
    j Default_IRQ_Handler         /* Slots 4..6 */
    .endr
    j riscv_mtimer_handler        /* Slot 7: CLINT Machine Timer Interrupt (SysTick equivalent) */
    .rept 24
    j Default_IRQ_Handler         /* Slots 8..31: External / Peripheral IRQs */
    .endr

.section .text
.global rtos_start_first_task
.type rtos_start_first_task, @function

/* a0 = current_tcb->sp */
rtos_start_first_task:
    mv    sp, a0
    lw    t0, 0(sp)               /* Pop initial mepc (task entry function) */
    csrw  mepc, t0
    lw    t0, 4(sp)               /* Pop initial mstatus (0x1880: MPP=3, MPIE=1) */
    csrw  mstatus, t0
    lw    ra, 8(sp)               /* Pop ra (rtos_task_exit_hook) */
    lw    a0, 36(sp)              /* Pop a0 (void *arg) */
    addi  sp, sp, 128             /* Release 32-word initial stack frame */
    mret                          /* Atomically sets MIE=MPIE and jumps to mepc! */

/* Macro to save all registers onto current task stack */
.macro SAVE_CONTEXT
    addi  sp, sp, -128
    sw    ra,  8(sp)
    sw    t0,  12(sp)
    sw    t1,  16(sp)
    sw    t2,  20(sp)
    sw    t3,  24(sp)
    sw    t4,  28(sp)
    sw    t5,  32(sp)
    sw    a0,  36(sp)
    sw    a1,  40(sp)
    sw    a2,  44(sp)
    sw    a3,  48(sp)
    sw    a4,  52(sp)
    sw    a5,  56(sp)
    sw    a6,  60(sp)
    sw    a7,  64(sp)
    sw    s0,  68(sp)
    sw    s1,  72(sp)
    sw    s2,  76(sp)
    sw    s3,  80(sp)
    sw    s4,  84(sp)
    sw    s5,  88(sp)
    sw    s6,  92(sp)
    sw    s7,  96(sp)
    sw    s8,  100(sp)
    sw    s9,  104(sp)
    sw    s10, 108(sp)
    sw    s11, 112(sp)
    sw    t6,  116(sp)
    csrr  t0,  mepc
    sw    t0,  0(sp)
    csrr  t0,  mstatus
    sw    t0,  4(sp)
.endm

/* Macro to restore all registers from current task stack */
.macro RESTORE_CONTEXT
    lw    t0,  0(sp)
    csrw  mepc, t0
    lw    t0,  4(sp)
    csrw  mstatus, t0
    lw    ra,  8(sp)
    lw    t0,  12(sp)
    lw    t1,  16(sp)
    lw    t2,  20(sp)
    lw    t3,  24(sp)
    lw    t4,  28(sp)
    lw    t5,  32(sp)
    lw    a0,  36(sp)
    lw    a1,  40(sp)
    lw    a2,  44(sp)
    lw    a3,  48(sp)
    lw    a4,  52(sp)
    lw    a5,  56(sp)
    lw    a6,  60(sp)
    lw    a7,  64(sp)
    lw    s0,  68(sp)
    lw    s1,  72(sp)
    lw    s2,  76(sp)
    lw    s3,  80(sp)
    lw    s4,  84(sp)
    lw    s5,  88(sp)
    lw    s6,  92(sp)
    lw    s7,  96(sp)
    lw    s8,  100(sp)
    lw    s9,  104(sp)
    lw    s10, 108(sp)
    lw    s11, 112(sp)
    lw    t6,  116(sp)
    addi  sp, sp, 128
.endm

/* Slot 3: CLINT Machine Software Interrupt (Context Switcher) */
.global riscv_msip_handler
riscv_msip_handler:
    SAVE_CONTEXT

    /* Clear CLINT MSIP register (0x20001C00 = 0) */
    li    t0, 0x20001C00
    sw    zero, 0(t0)

    /* Save outgoing sp to current_tcb->sp */
    la    t0, current_tcb
    lw    t1, 0(t0)
    sw    sp, 0(t1)

    /* Switch current_tcb = next_tcb */
    la    t2, next_tcb
    lw    t3, 0(t2)
    sw    t3, 0(t0)

    /* Load incoming sp from next_tcb->sp */
    lw    sp, 0(t3)

    RESTORE_CONTEXT
    mret

/* Slot 7: CLINT Machine Timer Interrupt (1000 Hz Tick) */
.global riscv_mtimer_handler
riscv_mtimer_handler:
    SAVE_CONTEXT

    /* Save outgoing sp to current_tcb->sp in case SysTick_Handler switches tasks */
    la    t0, current_tcb
    lw    t1, 0(t0)
    sw    sp, 0(t1)

    /* Advance MTIMECMP and run C SysTick_Handler() */
    call  bsp_ack_mtimer
    call  SysTick_Handler

    /* If next_tcb was updated by rtos_schedule(), switch to it immediately */
    la    t0, current_tcb
    lw    t1, 0(t0)
    la    t2, next_tcb
    lw    t3, 0(t2)
    beqz  t3, 1f
    sw    t3, 0(t0)
    mv    t1, t3
1:
    lw    sp, 0(t1)
    RESTORE_CONTEXT
    mret

riscv_exception_handler:
    mv    a0, sp
    call  rtos_fault_capture
Default_IRQ_Handler:
    j     Default_IRQ_Handler
```

---

### Step 4: Adapt the 3 Architecture Hooks in `kernel/rtos.c`

In `kernel/rtos.c`, add an `#elif defined(RTOS_ARCH_RISCV)` block in three places:

#### 4.1 Critical Sections & Software Interrupt Trigger (Top of `kernel/rtos.c`)
```c
#elif defined(RTOS_ARCH_RISCV)
#define CLINT_MSIP_REG    (*(volatile uint32_t *)0x20001C00UL)

static inline void rtos_enter_critical(void) {
    __asm volatile ("csrci mstatus, 0x8" : : : "memory");
}

static inline void rtos_exit_critical(void) {
    __asm volatile ("csrsi mstatus, 0x8" : : : "memory");
}

static inline void rtos_trigger_pendsv(void) {
    CLINT_MSIP_REG = 1UL; // Trigger RISC-V CLINT Machine Software Interrupt (Slot 3)
}
```

#### 4.2 Initial Stack Frame in `rtos_task_create()` (`kernel/rtos.c`)
```c
#ifdef RTOS_ARCH_RISCV
    // Align to 16 bytes per RISC-V ABI and allocate 32-word (128-byte) frame
    uint32_t *sp = (uint32_t *)(((uintptr_t)&stack_buf[stack_size_words - 1]) & ~0xFUL);
    sp -= 32;
    for (int i = 0; i < 32; i++) {
        sp[i] = 0;
    }
    sp[0] = (uint32_t)(uintptr_t)func;                // mepc: Task entry point
    sp[1] = 0x00001880UL;                             // mstatus: MPP=3 (M-mode), MPIE=1
    sp[2] = (uint32_t)(uintptr_t)rtos_task_exit_hook; // x1 (ra): Task exit hook
    sp[9] = (uint32_t)(uintptr_t)arg;                 // x10 (a0): void *arg
    t->sp = sp;
#else
    // Existing ARM Cortex-M4 16-word stack frame...
#endif
```

#### 4.3 Starting the Timer & First Task in `rtos_start()` (`kernel/rtos.c`)
```c
#ifdef RTOS_ARCH_RISCV
    extern void bsp_start_mtimer(uint32_t cpu_freq_hz, uint32_t tick_freq_hz);
    bsp_start_mtimer(cpu_freq_hz, tick_freq_hz);
    // Enable Machine Software Interrupt (MSIE = bit 3) & Machine Timer Interrupt (MTIE = bit 7)
    uint32_t mie_mask = (1UL << 3) | (1UL << 7);
    __asm volatile ("csrs mie, %0" : : "r"(mie_mask));
    rtos_start_first_task(current_tcb->sp);
#endif
```

---

### Step 5: Create the ESP32-C6-Pico BSP Header (`bsp/esp32c6/esp32c6.h`)

The **Waveshare ESP32-C6-Pico** connects its USB-C port directly to the ESP32-C6's internal **USB-Serial/JTAG Controller (`0x6000F000`)**. Writing bytes to `USB_DEVICE_EP1_REG` (`0x6000F000`) and setting `USB_DEVICE_SERIAL_WR_DONE` in `USB_DEVICE_EP1_CONF_REG` (`0x6000F004`) outputs directly to `/dev/ttyACM0` without needing any external UART-to-USB bridge or baud-rate dividers.

Create `bsp/esp32c6/esp32c6.h`:

```c
#ifndef ESP32C6_H
#define ESP32C6_H

#include <stdint.h>
#include <stdbool.h>
#include "rtos.h"

// 1. ESP32-C6 Watchdog Registers (Must be disabled at boot!)
#define TIMG0_BASE                  (0x60008000UL)
#define TIMG0_WDTCONFIG0_REG        (*(volatile uint32_t *)(TIMG0_BASE + 0x0048UL))
#define TIMG0_WDTWPROTECT_REG       (*(volatile uint32_t *)(TIMG0_BASE + 0x0064UL))

#define LP_WDT_BASE                 (0x600B1C00UL)
#define LP_WDT_CONFIG0_REG          (*(volatile uint32_t *)(LP_WDT_BASE + 0x0000UL))
#define LP_WDT_WPROTECT_REG         (*(volatile uint32_t *)(LP_WDT_BASE + 0x0018UL))
#define LP_WDT_SWD_CONFIG_REG       (*(volatile uint32_t *)(LP_WDT_BASE + 0x001CUL))
#define LP_WDT_SWD_WPROTECT_REG     (*(volatile uint32_t *)(LP_WDT_BASE + 0x0020UL))

// 2. ESP32-C6 CLINT Machine Timer Registers
#define CLINT_MTIME_LO              (*(volatile uint32_t *)0x20001800UL)
#define CLINT_MTIME_HI              (*(volatile uint32_t *)0x20001804UL)
#define CLINT_MTIMECMP_LO           (*(volatile uint32_t *)0x20001808UL)
#define CLINT_MTIMECMP_HI           (*(volatile uint32_t *)0x2000180CUL)

// 3. ESP32-C6 Built-in USB-Serial/JTAG Console Registers (/dev/ttyACM0)
#define USB_DEVICE_BASE             (0x6000F000UL)
#define USB_DEVICE_EP1_REG          (*(volatile uint32_t *)(USB_DEVICE_BASE + 0x0000UL))
#define USB_DEVICE_EP1_CONF_REG     (*(volatile uint32_t *)(USB_DEVICE_BASE + 0x0004UL))
#define USB_DEVICE_JTAG_ST_REG      (*(volatile uint32_t *)(USB_DEVICE_BASE + 0x0008UL))

// 4. ESP32-C6 GPIO Registers (GPIO8 = Onboard LED on ESP32-C6-Pico)
#define GPIO_BASE                   (0x60091000UL)
#define GPIO_OUT_REG                (*(volatile uint32_t *)(GPIO_BASE + 0x0004UL))
#define GPIO_OUT_W1TS_REG           (*(volatile uint32_t *)(GPIO_BASE + 0x0008UL))
#define GPIO_OUT_W1TC_REG           (*(volatile uint32_t *)(GPIO_BASE + 0x000CUL))
#define GPIO_ENABLE_W1TS_REG        (*(volatile uint32_t *)(GPIO_BASE + 0x0024UL))
#define LED_GPIO_PIN                8U

static uint32_t g_mtimer_step = 16000U; // Default 16 MHz CLINT timer / 1000 Hz = 16000

static inline void bsp_disable_watchdogs(void) {
    // Unlock & disable Timer Group 0 WDT
    TIMG0_WDTWPROTECT_REG = 0x50D83AA1UL;
    TIMG0_WDTCONFIG0_REG  = 0;
    // Unlock & disable RTC / LP WDT
    LP_WDT_WPROTECT_REG   = 0x50D83AA1UL;
    LP_WDT_CONFIG0_REG    = 0;
    // Unlock & auto-feed Super WDT
    LP_WDT_SWD_WPROTECT_REG = 0x50D83AA1UL;
    LP_WDT_SWD_CONFIG_REG  |= (1UL << 18); // SWD_AUTO_FEED_EN
}

static inline void bsp_ack_mtimer(void) {
    uint64_t now = ((uint64_t)CLINT_MTIME_HI << 32) | CLINT_MTIME_LO;
    uint64_t next = now + g_mtimer_step;
    CLINT_MTIMECMP_HI = 0xFFFFFFFFUL;
    CLINT_MTIMECMP_LO = (uint32_t)(next & 0xFFFFFFFFUL);
    CLINT_MTIMECMP_HI = (uint32_t)(next >> 32);
}

static inline void bsp_start_mtimer(uint32_t cpu_freq_hz, uint32_t tick_freq_hz) {
    g_mtimer_step = cpu_freq_hz / tick_freq_hz;
    bsp_ack_mtimer();
}

static inline void bsp_init_led(void) {
    GPIO_ENABLE_W1TS_REG = (1UL << LED_GPIO_PIN);
}

static inline void bsp_toggle_led(void) {
    if (GPIO_OUT_REG & (1UL << LED_GPIO_PIN)) {
        GPIO_OUT_W1TC_REG = (1UL << LED_GPIO_PIN);
    } else {
        GPIO_OUT_W1TS_REG = (1UL << LED_GPIO_PIN);
    }
}

static inline void bsp_set_led(bool on) {
    if (on) GPIO_OUT_W1TS_REG = (1UL << LED_GPIO_PIN);
    else    GPIO_OUT_W1TC_REG = (1UL << LED_GPIO_PIN);
}

static inline bool bsp_get_led(void) {
    return (GPIO_OUT_REG & (1UL << LED_GPIO_PIN)) != 0;
}

static inline void bsp_init_uart(uint32_t _cpu_freq_hz, uint32_t _baud) {
    (void)_cpu_freq_hz;
    (void)_baud; // USB-Serial/JTAG is pre-initialized by ESP32-C6 ROM
}

static inline void bsp_uart_putc(char c) {
    // Wait until USB-Serial TX FIFO has space (bit 1 of EP1_CONF_REG)
    while ((USB_DEVICE_EP1_CONF_REG & (1UL << 1)) == 0) {}
    USB_DEVICE_EP1_REG = (uint8_t)c;
    if (c == '\n') {
        USB_DEVICE_EP1_CONF_REG = (1UL << 0); // Flush USB packet on newline
    }
}

static inline bool bsp_uart_getc(char *out_c) {
    // Bit 0 of JTAG_ST_REG (SERIAL_OUT_EP_DATA_AVAIL) indicates RX byte ready
    if (USB_DEVICE_JTAG_ST_REG & (1UL << 0)) {
        *out_c = (char)(USB_DEVICE_EP1_REG & 0xFFU);
        return true;
    }
    return false;
}

#endif // ESP32C6_H
```

---

### Step 6: Add the `esp32c6` Target to `Makefile`

Add the following target block to `Makefile`:

```makefile
CFLAGS_ESP32C6 ?= --target=riscv32-unknown-elf -march=rv32imac_zicsr_zifencei -mabi=ilp32 \
                  -O2 -Wall -Wextra -ffreestanding -DRTOS_ARCH_RISCV \
                  -Wno-unused-parameter -Wno-unused-variable -Wno-unused-function \
                  -DZC_FUNC= -DZC_GLOBAL= \
                  -Ikernel/include -Ibsp/esp32c6 -Iarch/riscv/rv32

LDFLAGS_ESP32C6 ?= --target=riscv32-unknown-elf -march=rv32imac_zicsr_zifencei -mabi=ilp32 \
                   -nostdlib -fuse-ld=lld -Wl,-T,bsp/esp32c6/link.ld -Wl,--gc-sections

esp32c6: $(BUILD_DIR)/zen_rtos_esp32c6.bin

$(BUILD_DIR)/zen_rtos_esp32c6.elf: kernel/rtos.c \
                                   arch/riscv/rv32/context.s \
                                   bsp/esp32c6/startup.s \
                                   app/main_zc.c
	@mkdir -p $(BUILD_DIR)
	$(CLANG) $(CFLAGS_ESP32C6) $(LDFLAGS_ESP32C6) $^ -o $@

$(BUILD_DIR)/zen_rtos_esp32c6.bin: $(BUILD_DIR)/zen_rtos_esp32c6.elf
	$(LLVM_OBJCOPY) -O binary $< $@
```

---

## 4. Verification & Bring-Up Checklist on ESP32-C6-Pico

Follow these 6 phases in order as you implement the port:

1. **Phase 1 — Verify Toolchain Support**:
   - Run `clang --target=riscv32-unknown-elf -march=rv32imac_zicsr_zifencei -mabi=ilp32 -c kernel/rtos.c -Ikernel/include -DRTOS_ARCH_RISCV -o /tmp/rtos_rv32.o` to confirm your installed LLVM/Clang supports 32-bit RISC-V (`rv32imac`) out of the box.
2. **Phase 2 — Bare-Metal Boot & Watchdog Survival Test**:
   - Build `startup.s` + `esp32c6.h` with a simple `while(1)` loop printing `"Hello ESP32-C6!"` every second over `USB_DEVICE_EP1_REG`.
   - Load into SRAM via `esptool.py --chip esp32c6 load_ram build/zen_rtos_esp32c6.bin`.
   - Confirm that the board **does not reboot after 1.6 seconds** (proving `bsp_disable_watchdogs()` succeeded).
3. **Phase 3 — First Task Launch (`rtos_start_first_task` via `mret`)**:
   - Create a single task in `main.zc` and call `rtos_start(16000000, 1000)`.
   - Verify that `mret` jumps cleanly into `mepc` (`task_sensor_poll`) with `a0` containing `arg`.
4. **Phase 4 — Software Context Switch (`CLINT_MSIP_REG` Slot 3)**:
   - Call `rtos_yield()` between two tasks of equal priority and verify `riscv_msip_handler` saves and restores `sp` at offset `0` of `tcb_t`.
5. **Phase 5 — 1000 Hz `MTIME` Tick & Preemptive Delays (`Slot 7`)**:
   - Verify `rtos_delay(500)` wakes tasks accurately every `500 ms` and `g_system_ticks` increments at `1000 Hz`.
6. **Phase 6 — Full Multi-Tasking + `zensh>` Interactive Shell over USB-C**:
   - Open `make shell` on the ESP32-C6-Pico USB-C port and run `help`, `ps`, `mem`, `sensor`, and `trace` to confirm `Stack Canary Check (0xDEADBEEF): PASS`.
