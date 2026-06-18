.section .text
.code64

.extern syscall_dispatch
.extern log_info
.extern arch_enter_user_debug_log
.extern arch_enter_user_dump_stack

.global syscall_entry
syscall_entry:
    /* Preserve user context required for SYSRET.
     * x86-64 syscall ABI (SYSCALL):
     *   RAX=syscall_number
     *   RCX=RIP after syscall
     *   R11=RFLAGS after syscall
     *   RDI=a0, RSI=a1, RDX=a2, R10=a3, R8=a4, R9=a5
     */
    movq %rsp, saved_user_rsp(%rip)
    movq %rcx, saved_user_rip(%rip)
    movq %r11, saved_user_rflags(%rip)

    movq %rdi, saved_user_rdi(%rip) /* a0 */
    movq %rsi, saved_user_rsi(%rip) /* a1 */
    movq %rdx, saved_user_rdx(%rip) /* a2 */
    movq %rbx, saved_user_rbx(%rip)
    movq %rbp, saved_user_rbp(%rip)
    movq %r12, saved_user_r12(%rip)
    movq %r13, saved_user_r13(%rip)
    movq %r14, saved_user_r14(%rip)
    movq %r15, saved_user_r15(%rip)

    movq %r8,  saved_user_r8(%rip)  /* a4 */
    movq %r9,  saved_user_r9(%rip)  /* a5 */
    movq %r10, saved_user_r10(%rip) /* a3 */
    movq %rax, saved_syscall_number(%rip)

    /* Enter kernel data segments before touching the kernel stack. */
    movq $0x10, %rdx
    movw %dx, %ds
    movw %dx, %es

    /* Switch to a dedicated kernel stack for syscall handling. */
    movq $syscall_stack_top, %rsp

    /* Prepare SysV ABI arguments for:
     *   syscall_dispatch(uint64_t number, uint64_t a0, uint64_t a1, uint64_t a2,
     *                      uint64_t a3, uint64_t a4, uint64_t a5)
     *
     * SysV mapping:
     *   rdi=number, rsi=a0, rdx=a1, rcx=a2, r8=a3, r9=a4, [rsp]=a5 (7th arg)
     */
    movq saved_syscall_number(%rip), %rdi /* number */
    movq saved_user_rdi(%rip), %rsi       /* a0 */
    movq saved_user_rsi(%rip), %rdx       /* a1 */
    movq saved_user_rdx(%rip), %rcx       /* a2 */
    movq saved_user_r10(%rip), %r8        /* a3 (from original R10) */
    movq saved_user_r8(%rip), %r9         /* a4 (from original R8) */

    /* Place a5 (original R9) as the 7th argument on stack. */
    subq $16, %rsp
    movq saved_user_r9(%rip), %r10
    movq %r10, (%rsp)
    call syscall_dispatch
    addq $16, %rsp

    /* Decode syscall return disposition/scalar from C return value in RAX.
       Encoding (see syscall_disposition.c):
         upper bits[63:62] = disposition
         lower bits[61:0]  = signed scalar (62-bit sign-extended)
    */
    movq %rax, %rcx               /* rcx = encoded */
    shrq $62, %rcx               /* rcx = disposition */
    cmpq $1, %rcx
    je 1f

    /* Return-to-userspace: decode the 62-bit scalar placed in RAX. */
    shlq $2, %rax
    sarq $2, %rax

    movq saved_user_rsp(%rip), %rsp
    movq saved_user_rip(%rip), %rcx
    movq saved_user_rflags(%rip), %r11
    orq  $0x200, %r11      /* ensure IF is set in restored RFLAGS */

    xorq %rdx, %rdx
    movw %dx, %ds
    movw %dx, %es

    movq saved_user_rdi(%rip), %rdi
    movq saved_user_rsi(%rip), %rsi
    movq saved_user_rdx(%rip), %rdx
    movq saved_user_rbx(%rip), %rbx
    movq saved_user_rbp(%rip), %rbp
    movq saved_user_r12(%rip), %r12
    movq saved_user_r13(%rip), %r13
    movq saved_user_r14(%rip), %r14
    movq saved_user_r15(%rip), %r15
    movq saved_user_r8(%rip), %r8
    movq saved_user_r9(%rip), %r9
    movq saved_user_r10(%rip), %r10
    sysretq



1:
    /* Return-to-kernel: syscall result value is placed in rax for any kernel-side
       handling; exit path returns back to the saved shell context.
     * Use saved_exec_rsp — NOT saved_kernel_rsp — because saved_kernel_rsp is
     * overwritten on every syscall entry with syscall_stack_top, so by the time
     * SYS_EXIT fires saved_kernel_rsp no longer points at the arch_enter_user
     * frame. saved_exec_rsp is written only by arch_enter_user and is stable. */
    shlq $2, %rax
    sarq $2, %rax
    movq $0x10, %rdx

    movw %dx, %ds
    movw %dx, %es
    movq saved_exec_rsp(%rip), %rsp
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %rbp
    popq %rbx
    ret

.global arch_enter_user
arch_enter_user:
    /* rdi = ELF entry point, rsi = user stack pointer */
    pushq %rbx
    pushq %rbp
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15
    /*
     * Save the exec-caller kernel RSP in BOTH saved_kernel_rsp (used by the
     * syscall return-to-kernel path) AND saved_exec_rsp (used by
     * arch_abort_user from exception context, where saved_kernel_rsp may
     * have been overwritten by a syscall that was in progress).
     */
    movq %rsp, saved_kernel_rsp(%rip)
    movq %rsp, saved_exec_rsp(%rip)

    xorq %rdx, %rdx
    movw %dx, %ds
    movw %dx, %es

    /* iretq pops (in this order): RIP, CS, RFLAGS, RSP, SS
     * Push them in reverse order (last item pushed = first item popped) */
    pushq $0x1b          /* SS  = user data (5th pop) */
    pushq %rsi           /* RSP = user stack (4th pop) */
    pushq $0x202         /* RFLAGS: IF=1, reserved bit 1 (3rd pop) */
    pushq $0x23          /* CS  = user code (2nd pop) */
    pushq %rdi           /* RIP = entry point (1st pop) */
    iretq

.global arch_get_exec_rsp
arch_get_exec_rsp:
    movq saved_exec_rsp(%rip), %rax
    ret

.global arch_set_exec_rsp
arch_set_exec_rsp:
    movq %rdi, saved_exec_rsp(%rip)
    ret

.global arch_abort_user
arch_abort_user:
    /*
     * Abort a faulting ring-3 image and return to the kernel caller that
     * entered it through arch_enter_user().
     *
     * Use saved_exec_rsp — NOT saved_kernel_rsp — because saved_kernel_rsp
     * may have been overwritten by a syscall that was in progress when the
     * exception occurred (syscall_entry overwrites it with syscall_stack_top).
     * saved_exec_rsp is written only by arch_enter_user and is stable.
     *
     * rdi = process exit code to return from arch_enter_user()
     */
    movq %rdi, %rax
    movq $0x10, %rdx
    movw %dx, %ds
    movw %dx, %es
    movq saved_exec_rsp(%rip), %rsp
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %rbp
    popq %rbx
    ret

.section .bss
.align 16
saved_kernel_rsp:
    .quad 0
saved_exec_rsp:
    .quad 0
saved_user_rsp:
    .quad 0
saved_user_rip:
    .quad 0
saved_user_rflags:
    .quad 0
saved_user_rdi:
    .quad 0
saved_user_rsi:
    .quad 0
saved_user_rdx:
    .quad 0
saved_user_rbx:
    .quad 0
saved_user_rbp:
    .quad 0
saved_user_r12:
    .quad 0
saved_user_r13:
    .quad 0
saved_user_r14:
    .quad 0
saved_user_r15:
    .quad 0
saved_user_r8:
    .quad 0
saved_user_r9:
    .quad 0
saved_user_r10:
    .quad 0
saved_syscall_number:
    .quad 0
saved_arg5:
    .quad 0

.align 16
syscall_stack_bottom:
    .skip 262144
syscall_stack_top:
