.section .direct_boot_header, "a"
.global _direct_boot_magic
_direct_boot_magic:
    .word 0xAEDB041D
    .word 0xAEDB041D

.section .init, "ax"
.global _start
.type _start, @function

.weak __global_pointer$
.weak _siiram
.weak _siram
.weak _eiram

_start:
    /* 1. Disable global machine interrupts (mstatus.MIE = 0) */
    csrci mstatus, 0x8

    /* 2. Initialize RISC-V Global Pointer (gp) if defined by linker */
    .option push
    .option norelax
    la    gp, __global_pointer$
    .option pop

    /* 3. Initialize Stack Pointer (sp) to _estack */
    la    sp, _estack

    /* 4. Copy .iram1 from Flash XIP into HP SRAM (no-op when _siiram == _siram) */
    la    t0, _siram
    la    t1, _eiram
    la    t2, _siiram
    beq   t0, t2, 2f
1:
    bgeu  t0, t1, 2f
    lw    t3, 0(t2)
    sw    t3, 0(t0)
    addi  t0, t0, 4
    addi  t2, t2, 4
    j     1b
2:
    /* 5. Point mtvec to our 256-byte-aligned RISC-V Vectored Trap Table (mode = 1) */
    la    t0, _mtvec_table
    ori   t0, t0, 1
    csrw  mtvec, t0

    /* 6. Copy .data from Flash XIP into HP SRAM (no-op when _sidata == _sdata) */
    la    t0, _sdata
    la    t1, _edata
    la    t2, _sidata
    beq   t0, t2, 4f
3:
    bgeu  t0, t1, 4f
    lw    t3, 0(t2)
    sw    t3, 0(t0)
    addi  t0, t0, 4
    addi  t2, t2, 4
    j     3b
4:
    /* 7. Zero-initialize .bss (_sbss .. _ebss) */
    la    t0, _sbss
    la    t1, _ebss
5:
    bgeu  t0, t1, 6f
    sw    zero, 0(t0)
    addi  t0, t0, 4
    j     5b
6:
    /* 8. Disable ESP32-C6 hardware watchdog timers, then call main() */
    call  bsp_disable_watchdogs
    call  main

7:
    j     7b
