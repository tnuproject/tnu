ENTRY(_start)

PHDRS
{
    text PT_LOAD FLAGS(5);
    rodata PT_LOAD FLAGS(4);
    data PT_LOAD FLAGS(6);
}

SECTIONS
{
    . = 1M;

    .text ALIGN(4K) : {
        KEEP(*(.multiboot))
        *(.text.boot)
        *(.text*)
    } :text

    .rodata ALIGN(4K) : {
        *(.rodata*)
        *(.eh_frame*)
    } :rodata

    .data ALIGN(4K) : {
        *(.data*)
    } :data

    .bss ALIGN(4K) : {
        *(COMMON)
        *(.bss*)
    } :data

    kernel_end = .;

    /DISCARD/ : {
        *(.note*)
        *(.comment)
    }
}
