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
    la  sp, _estack

    /* 3. Point mtvec to our 256-byte-aligned RISC-V Vectored Trap Table (mode = 1) */
    la t0, _mtvec_table
    ori t0, t0, 1
    csrw mtvec, t0

    /* 4. Zero-inialize .bss(_sbss .. _ebss) */
    la t0, _sbss
    la t1, _ebss

1:
    bgeu    t0, t1, 2f
    sw      zero, 0(t0)
    addi    t0, t0, 4
    j       1b

2:
    /* 5. Disable ESP32-C6 hardware watchod timers in c helper, then call main() */
    call bsp_disable_watchdogs
    call main

3: j 3b


