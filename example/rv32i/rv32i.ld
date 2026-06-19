OUTPUT_ARCH(riscv)
ENTRY(_start)

MEMORY
{
    imem (rx) : ORIGIN = 0x00000000, LENGTH = 64K
    dmem (rw) : ORIGIN = 0x00010000, LENGTH = 64K
}

SECTIONS
{
    .text : {
        *(.text._start)
        *(.text*)
    } > imem

    .rodata : {
        *(.rodata*)
    } > imem

    .data : {
        *(.data*)
    } > dmem

    .bss : {
        *(.bss*)
    } > dmem

    /DISCARD/ : {
        *(.comment*)
        *(.riscv.attributes)
    }
}
