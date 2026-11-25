/* Linker script for NXP i.MX RT1021 (RAM Boot: OCRAM code + DTCM data/stacks) */
/* Default FlexRAM split: 64KB ITCM @ 0x00000000, 64KB DTCM @ 0x20000000, 128KB OCRAM @ 0x20200000 */

ENTRY(Reset_Handler)

MEMORY
{
    ITCM  (rwx) : ORIGIN = 0x00000400, LENGTH = 63K
    DTCM  (rwx) : ORIGIN = 0x20000000, LENGTH = 64K
    OCRAM (rwx) : ORIGIN = 0x20200000, LENGTH = 128K
}

_estack = ORIGIN(DTCM) + LENGTH(DTCM);  /* Top of 64KB DTCM (0x20010000) */

SECTIONS
{
    /* Cortex-M7 vector table (1024-byte aligned for SCB->VTOR) */
    .isr_vector :
    {
        . = ALIGN(1024);
        KEEP(*(.isr_vector))
        . = ALIGN(4);
    } > OCRAM

    /* Program code and read-only data */
    .text :
    {
        . = ALIGN(4);
        *(.text)
        *(.text*)
        *(.rodata)
        *(.rodata*)
        . = ALIGN(4);
        _etext = .;
    } > OCRAM

    /* Initialized data copied from OCRAM LMA into zero-wait-state DTCM */
    _sidata = LOADADDR(.data);

    .data :
    {
        . = ALIGN(4);
        _sdata = .;
        *(.data)
        *(.data*)
        . = ALIGN(4);
        _edata = .;
    } > DTCM AT > OCRAM

    /* Uninitialized data, TCBs and task stacks in DTCM */
    .bss :
    {
        . = ALIGN(4);
        _sbss = .;
        *(.bss)
        *(.bss*)
        *(COMMON)
        . = ALIGN(4);
        _ebss = .;
    } > DTCM

    /DISCARD/ :
    {
        *(.ARM.attributes)
        *(.comment)
    }
}
