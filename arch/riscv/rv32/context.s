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

    /* Clear CLINT MSIP register (0x20001800 = 0) */
    li    t0, 0x20001800
    sw    zero, 0(t0)

    /* Save outgoing sp to current_tcb->sp */
    la    t0, current_tcb
    lw    t1, 0(t0)
    sw    sp, 0(t1)

    /* Switch current_tcb = next_tcb (if non-null) */
    la    t2, next_tcb
    lw    t3, 0(t2)
    beqz  t3, 1f
    sw    zero, 0(t2)
    sw    t3, 0(t0)
    mv    t1, t3
1:
    /* Load incoming sp from current_tcb->sp */
    lw    sp, 0(t1)

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
    sw    zero, 0(t2)
    sw    t3, 0(t0)
    mv    t1, t3
    /* Also clear pending MSIP (0x20001800) since we already switched here */
    li    t4, 0x20001800
    sw    zero, 0(t4)
1:
    lw    sp, 0(t1)
    RESTORE_CONTEXT
    mret

riscv_exception_handler:
    mv    a0, sp
    call  rtos_fault_capture
Default_IRQ_Handler:
    j     Default_IRQ_Handler

