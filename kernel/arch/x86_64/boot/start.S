.section .multiboot,"a"
.align 8
mb_header_start:
    .long 0xe85250d6
    .long 0
    .long mb_header_end - mb_header_start
    .long -(0xe85250d6 + 0 + (mb_header_end - mb_header_start))

    /* Ask GRUB to provide the memory tags the kernel consumes. */
    .align 8
    .short 1
    .short 0
    .long 20
    .long 4
    .long 6
    .long 8

    /* Prefer a linear framebuffer on UEFI/GOP, but keep booting if unavailable. */
    .align 8
    .short 5
    .short 1
    .long 20
    .long 1024
    .long 768
    .long 32

    /* Console information is useful, but not required for UEFI-only machines. */
    .align 8
    .short 4
    .short 1
    .long 12
    .long 0

    .align 8
    .short 0
    .short 0
    .long 8
mb_header_end:

.section .text
.global _start
.extern arch_entry

.code32
_start:
    cli
    movl %eax, mb_magic
    movl %ebx, mb_info

    call setup_page_tables
    call enable_long_mode
    lgdt gdt64_ptr
    ljmp $0x08, $long_mode_entry

setup_page_tables:
    movl $pdpt_table, %eax
    orl $0x3, %eax
    movl %eax, pml4_table
    movl $0, pml4_table + 4

    movl $pd_table, %eax
    orl $0x3, %eax
    movl %eax, pdpt_table
    movl $0, pdpt_table + 4

    xorl %ecx, %ecx
1:
    movl %ecx, %eax
    shll $21, %eax
    orl $0x83, %eax
    movl %eax, pd_table(,%ecx,8)
    movl $0, pd_table + 4(,%ecx,8)
    incl %ecx
    cmpl $512, %ecx
    jne 1b
    ret

enable_long_mode:
    movl %cr4, %eax
    orl $0x20, %eax
    movl %eax, %cr4

    movl $pml4_table, %eax
    movl %eax, %cr3

    movl $0xc0000080, %ecx
    rdmsr
    orl $0x100, %eax
    wrmsr

    movl %cr0, %eax
    orl $0x80000001, %eax
    movl %eax, %cr0
    ret

.code64
long_mode_entry:
    movw $0x10, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %ss
    movw %ax, %fs
    movw %ax, %gs

    movq $stack_top, %rsp
    xorq %rbp, %rbp

    movl mb_magic(%rip), %edi
    movl mb_info(%rip), %esi
    call arch_entry

1:
    hlt
    jmp 1b

.section .rodata
.align 8
gdt64:
    .quad 0x0000000000000000
    .quad 0x00af9a000000ffff
    .quad 0x00af92000000ffff
gdt64_end:
gdt64_ptr:
    .word gdt64_end - gdt64 - 1
    .long gdt64

.section .bss
.align 16
mb_magic:
    .skip 4
mb_info:
    .skip 4

.align 4096
.global pml4_table
pml4_table:
    .skip 4096
.global pdpt_table
pdpt_table:
    .skip 4096
.global pd_table
pd_table:
    .skip 4096

.align 16
stack_bottom:
    .skip 32768
.global stack_top
stack_top:
