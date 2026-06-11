.section .text
.global _start
.extern main

_start:
    xorq %rbp, %rbp
    movq (%rsp), %rdi
    leaq 8(%rsp), %rsi
    call main
    movq %rax, %rdi
    movq $7, %rax
    syscall
1:
    hlt
    jmp 1b
