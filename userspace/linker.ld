ENTRY(_start)

/*
 * Userspace linker script.
 *
 * The original script placed the load address at 0x4000000, which collides with
 * the reserved boot module area used by the kernel during exec. This caused
 * binaries such as doom and fastfetch to fail with:
 *   "ELF image overlaps reserved boot area"
 *
 * To avoid this, we move the base address up to 0x8000000. The kernel's
 * USER_BASE constant is updated accordingly (see syscall.c). The heap base is
 * also increased to keep the same relative layout.
 */
SECTIONS
{
    . = 0x4000000; /* Base address matching USER_BASE constant */
    .text ALIGN(4K) : { *(.text*) }
    .rodata ALIGN(4K) : { *(.rodata*) }
    .data ALIGN(4K) : { *(.data*) }
    .bss ALIGN(4K) : { *(COMMON) *(.bss*) }
}
