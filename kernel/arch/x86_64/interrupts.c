#include <arch/cpu.h>
#include <arch/idt.h>
#include <arch/keyboard.h>
#include <arch/pic.h>
#include <arch/pit.h>
#include <tnu/log.h>
#include <tnu/panic.h>
#include <tnu/scheduler.h>

static const char *exception_names[32] = {
    "divide error",
    "debug",
    "non-maskable interrupt",
    "breakpoint",
    "overflow",
    "bound range exceeded",
    "invalid opcode",
    "device not available",
    "double fault",
    "coprocessor segment overrun",
    "invalid TSS",
    "segment not present",
    "stack fault",
    "general protection fault",
    "page fault",
    "reserved",
    "x87 floating-point exception",
    "alignment check",
    "machine check",
    "SIMD floating-point exception",
    "virtualization exception",
    "control protection exception",
    "reserved",
    "reserved",
    "reserved",
    "reserved",
    "reserved",
    "reserved",
    "hypervisor injection exception",
    "VMM communication exception",
    "security exception",
    "reserved",
};

void isr_dispatch(struct interrupt_frame *frame)
{
    if (frame->vector < 32) {
        panic_exception(frame, exception_names[frame->vector]);
    }

    uint8_t irq = (uint8_t)(frame->vector - 32);
    switch (irq) {
    case 0:
        pit_tick();
        scheduler_tick();
        break;
    case 1:
        keyboard_handle_irq();
        break;
    default:
        log_debug("irq", "unhandled irq %u", irq);
        break;
    }
    pic_send_eoi(irq);
}
