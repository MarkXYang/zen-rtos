/* Linker script for STM32L476RG (1024KB Flash, 96KB SRAM1, 32KB SRAM2) */

ENTRY(Reset_Handler)

MEMORY
{
    FLASH (rx)      : ORIGIN = 0x08000000, LENGTH = 1024K
    RAM   (xrw)     : ORIGIN = 0x20000000, LENGTH = 96K
    RAM2  (xrw)     : ORIGIN = 0x10000000, LENGTH = 32K
}

_estack = ORIGIN(RAM) + LENGTH(RAM);    /* End of SRAM1 (Top of stack) */

SECTIONS
{
    /* The vector table must be at the beginning of Flash */
    .isr_vector :
    {
        . = ALIGN(4);
        KEEP(*(.isr_vector))
        . = ALIGN(4);
    } > FLASH

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
    } > FLASH

    /* Used by startup to copy initialized data to RAM */
    _sidata = LOADADDR(.data);

    /* Initialized data placed in RAM */
    .data :
    {
        . = ALIGN(4);
        _sdata = .;
        *(.data)
        *(.data*)
        . = ALIGN(4);
        _edata = .;
    } > RAM AT > FLASH

    /* Uninitialized data in RAM */
    .bss :
    {
        . = ALIGN(4);
        _sbss = .;
        *(.bss)
        *(.bss*)
        *(COMMON)
        . = ALIGN(4);
        _ebss = .;
    } > RAM

    /* Discard unused sections */
    /DISCARD/ :
    {
        *(.ARM.attributes)
    }
}
