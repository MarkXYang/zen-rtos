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
        KEEP(*(.direct_boot_header)) /* 0x40800000: 8-byte magic 0xAEDB041D, 0xAEDB041D */
        KEEP(*(.init))              /* 0x40800008: _start entry point at offset +0x08 */
        . = ALIGN(256);
        KEEP(*(.mtvec_table))       /* 0x40800100: 256-byte aligned RISC-V trap table */
        *(.iram1)
        *(.iram1.*)
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
        *(.got)
        *(.got.*)
        . = ALIGN(4);
        _edata = .;
    } > SRAM

    PROVIDE(__global_pointer$ = _sdata + 0x800);

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
