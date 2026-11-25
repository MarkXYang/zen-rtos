/* Linker script for NXP i.MX RT1021 (FlexSPI QSPI NOR XIP @ 0x60000000 + DTCM @ 0x20000000) */
/* Targets MIMXRT1020-EVK (8MB IS25LP064A QSPI NOR, 256KB FlexRAM: 64K ITCM / 64K DTCM / 128K OCRAM) */

ENTRY(Reset_Handler)

MEMORY
{
    FLASH (rx)  : ORIGIN = 0x60000000, LENGTH = 8M
    ITCM  (rwx) : ORIGIN = 0x00000400, LENGTH = 63K
    DTCM  (rwx) : ORIGIN = 0x20000000, LENGTH = 64K
    OCRAM (rwx) : ORIGIN = 0x20200000, LENGTH = 128K
}

_estack = ORIGIN(DTCM) + LENGTH(DTCM);  /* Top of 64KB DTCM (0x20010000) */

SECTIONS
{
    /* NXP i.MX RT1021 Boot ROM reads FlexSPI Configuration Block (FCB) at +0x400 */
    .boot_hdr.conf 0x60000400 :
    {
        KEEP(*(.boot_hdr.conf))
    } > FLASH

    /* NXP i.MX RT1021 Boot ROM reads Image Vector Table (IVT) & BootData at +0x1000 */
    .boot_hdr.ivt 0x60001000 :
    {
        KEEP(*(.boot_hdr.ivt))
        KEEP(*(.boot_hdr.boot_data))
        KEEP(*(.boot_hdr.dcd_data))
    } > FLASH

    /* Cortex-M7 vector table at +0x2000 (1024-byte aligned for SCB->VTOR) */
    .isr_vector 0x60002000 :
    {
        . = ALIGN(1024);
        KEEP(*(.isr_vector))
        . = ALIGN(4);
    } > FLASH

    /* Program code and read-only data executing in place (XIP) from FlexSPI NOR */
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

    /* Initialized data stored in Flash and copied to zero-wait-state DTCM by Reset_Handler */
    _sidata = LOADADDR(.data);

    .data :
    {
        . = ALIGN(4);
        _sdata = .;
        *(.data)
        *(.data*)
        . = ALIGN(4);
        _edata = .;
    } > DTCM AT > FLASH

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
