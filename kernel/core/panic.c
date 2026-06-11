#include <arch/cpu.h>
#include <arch/idt.h>
#include <tnu/console.h>
#include <tnu/panic.h>
#include <tnu/printf.h>
#include <tnu/version.h>

static void stack_trace(void)
{
    void **rbp;
    __asm__ volatile("movq %%rbp, %0" : "=r"(rbp));
    kprintf("\nStack trace:\n");
    for (int i = 0; i < 8 && rbp; i++) {
        void *ret = rbp[1];
        if (!ret) {
            break;
        }
        kprintf("  #%d %p\n", i, ret);
        rbp = (void **)rbp[0];
    }
}

static void panic_header(const char *message)
{
    cpu_cli();
    console_set_color(CONSOLE_WHITE, CONSOLE_BLUE);
    console_clear();
    kprintf("TNU KERNEL PANIC\n");
    kprintf("%s %s \"%s\" (%s)\n\n", TNU_NAME, TNU_VERSION, TNU_CODENAME, TNU_ARCH);
    kprintf("Debugging message: %s\n", message);
}

__attribute__((noreturn)) void panic(const char *message)
{
    panic_header(message);
    kprintf("Error code: 0x0000000000000000\n");
    stack_trace();
    kprintf("\nThe kernel stopped to protect the system. Check serial logs for context.\n");
    for (;;) {
        cpu_halt();
    }
}

__attribute__((noreturn)) void panic_exception(struct interrupt_frame *frame, const char *message)
{
    panic_header(message);
    kprintf("Error code: 0x%llx  Vector: %llu\n", frame->error, frame->vector);
    if (frame->vector == 14) {
        kprintf("CR2: 0x%llx\n", cpu_read_cr2());
    }
    kprintf("\nCPU registers:\n");
    kprintf("RAX=%016llx RBX=%016llx RCX=%016llx RDX=%016llx\n",
            frame->rax, frame->rbx, frame->rcx, frame->rdx);
    kprintf("RSI=%016llx RDI=%016llx RBP=%016llx RSP~=%016llx\n",
            frame->rsi, frame->rdi, frame->rbp, (uint64_t)&frame->rip);
    kprintf("R8 =%016llx R9 =%016llx R10=%016llx R11=%016llx\n",
            frame->r8, frame->r9, frame->r10, frame->r11);
    kprintf("R12=%016llx R13=%016llx R14=%016llx R15=%016llx\n",
            frame->r12, frame->r13, frame->r14, frame->r15);
    kprintf("RIP=%016llx CS=%04llx RFLAGS=%016llx\n",
            frame->rip, frame->cs, frame->rflags);
    stack_trace();
    kprintf("\nHelpful hint: inspect the exception vector and the last log lines in dmesg.\n");
    for (;;) {
        cpu_halt();
    }
}
