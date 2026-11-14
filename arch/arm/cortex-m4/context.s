.syntax unified
.cpu cortex-m4
.thumb

.global PendSV_Handler
.type PendSV_Handler, %function

PendSV_Handler:
    cpsid i                 /* Disable interrupts during TCB pointer swap */

    /* 1. Save outgoing task context */
    mrs   r0, psp           /* r0 = current Process Stack Pointer */
    stmdb r0!, {r4-r11}     /* Push R4-R11 onto task stack */

    ldr   r1, =current_tcb  /* r1 = &current_tcb */
    ldr   r2, [r1]          /* r2 = current_tcb */
    str   r0, [r2]          /* current_tcb->sp = r0 */

    /* 2. Switch current_tcb to next_tcb */
    ldr   r3, =next_tcb     /* r3 = &next_tcb */
    ldr   r4, [r3]          /* r4 = next_tcb */
    str   r4, [r1]          /* current_tcb = next_tcb */

    /* 3. Restore incoming task context */
    ldr   r0, [r4]          /* r0 = next_tcb->sp */
    ldmia r0!, {r4-r11}     /* Pop R4-R11 from new task stack */
    msr   psp, r0           /* Set PSP to incoming task stack */

    cpsie i                 /* Re-enable interrupts */
    ldr   lr, =0xFFFFFFFD   /* Return to Thread mode, use PSP */
    bx    lr                /* Hardware auto-pops R0-R3, R12, LR, PC, xPSR */

.global rtos_start_first_task
.type rtos_start_first_task, %function

rtos_start_first_task:
    /* r0 = current_tcb->sp */
    ldmia r0!, {r4-r11}     /* Pop initial R4-R11 */
    msr   psp, r0           /* Set PSP to top of hardware frame */
    mov   r1, #2            /* Bit 1 = Use PSP for Thread mode */
    msr   control, r1
    isb
    cpsie i                 /* Enable interrupts */
    pop   {r0-r3, r12, lr}  /* Pop hardware exception frame */
    pop   {pc}              /* Jump directly into task function */

/* -------------------------------------------------------------------------
 * Cortex-M4 Fault Analyzer Entry (HardFault / MemManage / BusFault / UsageFault)
 * Determines whether fault occurred on MSP or PSP (EXC_RETURN bit 2 in LR)
 * and passes the stacked exception frame pointer to rtos_fault_capture(sp).
 * ------------------------------------------------------------------------- */
.global HardFault_Handler
.type HardFault_Handler, %function
.global MemManage_Handler
.type MemManage_Handler, %function
.global BusFault_Handler
.type BusFault_Handler, %function
.global UsageFault_Handler
.type UsageFault_Handler, %function

HardFault_Handler:
MemManage_Handler:
BusFault_Handler:
UsageFault_Handler:
    cpsid i
    tst   lr, #4
    ite   eq
    mrseq r0, msp
    mrsne r0, psp
    b     rtos_fault_capture
