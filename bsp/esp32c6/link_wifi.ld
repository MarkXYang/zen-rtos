/* Linker script for ESP32-C6 Direct-Boot Flash XIP (0x42000000) + HP SRAM (0x40800000) */
/* Used by make esp32c6-wifi (flashed raw to SPI Flash @ 0x00000000 with 0xAEDB041D header) */

ENTRY(_start)

MEMORY
{
    /* ESP32-C6 Cache-mapped Flash XIP (Direct-Boot maps SPI Flash 0x0000 -> 0x42000000) */
    FLASH (rx)  : ORIGIN = 0x42000000, LENGTH = 4M
    /* ESP32-C6 496 KB usable High-Performance SRAM (top 16 KB reserved for ROM) */
    SRAM  (rwx) : ORIGIN = 0x40800000, LENGTH = 496K
}

_estack = 0x4087C000;

SECTIONS
{
    /* Direct-Boot header + _start entry at Flash offset +0x0000 / +0x0008 */
    .text :
    {
        . = ALIGN(4);
        KEEP(*(.direct_boot_header)) /* 0x42000000: 0xAEDB041D, 0xAEDB041D */
        KEEP(*(.init))               /* 0x42000008: _start                 */
        *(.text)
        *(.text*)
        *(.rodata)
        *(.rodata*)
        *(.rodata.wdev*)
        *(.srodata)
        *(.srodata*)
        . = ALIGN(4);
        _etext = .;
    } > FLASH

    /* Fast-path IRAM: trap vector table, context switch, ISRs & Wi-Fi IRAM hot paths */
    _siiram = LOADADDR(.iram1);
    .iram1 :
    {
        . = ALIGN(256);
        _siram = .;
        KEEP(*(.mtvec_table))        /* 256-byte aligned RISC-V trap table in HP SRAM */
        *(.iram1)
        *(.iram1.*)
        *(.wifi0iram)
        *(.wifi0iram.*)
        *(.wifirxiram)
        *(.wifirxiram.*)
        *(.wifiextrairam)
        *(.wifiextrairam.*)
        *(.wifislpiram)
        *(.wifislpiram.*)
        *(.wifislprxiram)
        *(.wifislprxiram.*)
        *(.phyiram)
        *(.phyiram.*)
        *(.coexiram)
        *(.coexiram.*)
        . = ALIGN(4);
        _eiram = .;
    } > SRAM AT > FLASH

    /* Initialized data copied from Flash XIP to HP SRAM */
    _sidata = LOADADDR(.data);
    .data :
    {
        . = ALIGN(4);
        _sdata = .;
        *(.data)
        *(.data*)
        *(.dram1)
        *(.dram1.*)
        *(.sdata)
        *(.sdata*)
        *(.got)
        *(.got.*)
        . = ALIGN(4);
        _edata = .;
    } > SRAM AT > FLASH

    /* Global pointer centered for +/-2 KB gp-relative .sdata/.sbss accesses */
    PROVIDE(__global_pointer$ = _sdata + 0x800);

    /* Uninitialized data, 4KB ISR stack, 192KB Wi-Fi heap & ZenRTOS TCBs */
    .bss (NOLOAD) :
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

    /DISCARD/ :
    {
        *(.comment)
        *(.riscv.attributes)
    }
}
