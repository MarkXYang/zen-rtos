.syntax unified
.cpu cortex-m7
.thumb

.global g_pfnVectors
.global Reset_Handler
.global Default_Handler

/* Linker symbols */
.extern _sidata
.extern _sdata
.extern _edata
.extern _sbss
.extern _ebss
.extern _estack
.extern bsp_early_init
.extern main

.section .text.Reset_Handler
.type Reset_Handler, %function
Reset_Handler:
    /* 1. Disable interrupts and initialize MSP to top of DTCM */
    cpsid i
    ldr   sp, =_estack

    /* 2. Relocate SCB->VTOR (0xE000ED08) to our Cortex-M7 vector table */
    ldr   r0, =0xE000ED08
    ldr   r1, =g_pfnVectors
    str   r1, [r0]
    dsb
    isb

    /* 3. Disable RTWDOG/WDOG1 and configure CCM clocks before memory init */
    bl    bsp_early_init

    /* 4. Copy .data segment from Flash/OCRAM LMA into DTCM */
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

    /* 5. Zero-initialize .bss segment in DTCM */
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

    /* 6. Enter Zen-C application main() */
    bl    main

LoopForever:
    b     LoopForever

.section .text.Default_Handler,"ax",%progbits
.type Default_Handler, %function
Default_Handler:
Infinite_Loop:
    b     Infinite_Loop

/* Cortex-M7 Vector Table (1024-byte aligned for SCB->VTOR) */
.section .isr_vector,"a",%progbits
.balign 1024
.type g_pfnVectors, %object

g_pfnVectors:
    /* Standard Cortex-M7 Core Exceptions (Slots 0..15) */
    .word _estack
    .word Reset_Handler
    .word NMI_Handler
    .word HardFault_Handler
    .word MemManage_Handler
    .word BusFault_Handler
    .word UsageFault_Handler
    .word 0
    .word 0
    .word 0
    .word 0
    .word SVC_Handler
    .word DebugMon_Handler
    .word 0
    .word PendSV_Handler
    .word SysTick_Handler

    /* MIMXRT1021 Peripheral IRQs 0..19 */
    .rept 20
    .word Default_Handler
    .endr

    /* Peripheral IRQ 20 (Vector Slot 36): LPUART1_IRQHandler */
    .word LPUART1_IRQHandler

    /* Remaining MIMXRT1021 Peripheral IRQs 21..159 */
    .rept 139
    .word Default_Handler
    .endr

/* Weak default aliases */
.weak LPUART1_IRQHandler
.thumb_set LPUART1_IRQHandler,Default_Handler

.weak NMI_Handler
.thumb_set NMI_Handler,Default_Handler

.weak HardFault_Handler
.thumb_set HardFault_Handler,Default_Handler

.weak MemManage_Handler
.thumb_set MemManage_Handler,Default_Handler

.weak BusFault_Handler
.thumb_set BusFault_Handler,Default_Handler

.weak UsageFault_Handler
.thumb_set UsageFault_Handler,Default_Handler

.weak SVC_Handler
.thumb_set SVC_Handler,Default_Handler

.weak DebugMon_Handler
.thumb_set DebugMon_Handler,Default_Handler
