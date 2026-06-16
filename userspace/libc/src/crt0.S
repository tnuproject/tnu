.section .text
.global _start
.extern main
.extern exit

_start:
    xorq %rbp, %rbp
    movq (%rsp), %rdi
    leaq 8(%rsp), %rsi
    andq $-16, %rsp
    call main
    movq %rax, %rdi
    call exit
1:
    hlt
    jmp 1b
