#include <arch/cpu.h>
#include <arch/idt.h>
#include <arch/keyboard.h>
#include <arch/pic.h>
#include <arch/pit.h>
#include <arch/serial.h>
#include <tnu/iwlwifi.h>
#include <tnu/log.h>
#include <tnu/panic.h>
#include <tnu/process.h>
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

/* CS selector for user code ring 3: low 2 bits are the RPL */
#define USER_CS_RPL 3

static bool frame_from_userspace(const struct interrupt_frame *frame);
static void wifi_handle_irq(void);

static bool frame_from_userspace(const struct interrupt_frame *frame)
{
    return (frame->cs & 3) == USER_CS_RPL;
}

static void kill_user_process(struct interrupt_frame *frame, const char *reason)
{
    struct process *proc = process_current();
    const char *name = proc ? proc->name : "<unknown>";

    /* Log to serial unconditionally — safe even during a nested fault */
    serial_write("exception: killing user process [");
    serial_write(name);
    serial_write("] reason=");
    serial_write(reason);
    serial_write(" rip=0x");
    /* Print RIP hex digits */
    uint64_t rip = frame->rip;
    char buf[17];
    int i = 15;
    buf[16] = '\0';
    do { buf[i--] = "0123456789abcdef"[rip & 0xf]; rip >>= 4; } while (rip && i >= 0);
    while (i >= 0) { buf[i--] = '0'; }
    serial_write(buf);
    if (frame->vector == 14) {
        serial_write(" cr2=0x");
        uint64_t cr2 = cpu_read_cr2();
        i = 15;
        do { buf[i--] = "0123456789abcdef"[cr2 & 0xf]; cr2 >>= 4; } while (cr2 && i >= 0);
        while (i >= 0) { buf[i--] = '0'; }
        serial_write(buf);
    }
    serial_write(" err=0x");
    uint64_t err = frame->error;
    i = 15;
    do { buf[i--] = "0123456789abcdef"[err & 0xf]; err >>= 4; } while (err && i >= 0);
    while (i >= 0) { buf[i--] = '0'; }
    serial_write(buf);
    serial_write("\n");

    if (proc) {
        process_exit(proc, 139);
    }

    arch_abort_user(139);
}

void keyboard_handle_irq(void);

void wifi_handle_irq(void)
{
    iwlwifi_poll_rx_notifications_all();
}

void isr_dispatch(struct interrupt_frame *frame)
{
    if (frame->vector < 32) {
        if (frame_from_userspace(frame)) {
            kill_user_process(frame, exception_names[frame->vector]);
            /* kill_user_process calls panic_exception which does not return */
        }
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
    case 11:
        wifi_handle_irq();
        break;
    default:
        log_debug("irq", "unhandled irq %u", irq);
        break;
    }
    pic_send_eoi(irq);
}
