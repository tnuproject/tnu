#include <stdint.h>
#include <tnu/log.h>

void arch_enter_user_debug_log(uint64_t entry, uint64_t stack) {
    log_info("arch", "arch_enter_user: entry=0x%lx stack=0x%lx", 
             (unsigned long)entry, (unsigned long)stack);
}

void arch_enter_user_dump_stack(uint64_t *stack_ptr) {
    log_info("arch", "iretq stack dump:");
    log_info("arch", "  [rsp+0]  RIP   = 0x%016lx", (unsigned long)stack_ptr[0]);
    log_info("arch", "  [rsp+8]  CS    = 0x%016lx", (unsigned long)stack_ptr[1]);
    log_info("arch", "  [rsp+16] RFLAGS= 0x%016lx", (unsigned long)stack_ptr[2]);
    log_info("arch", "  [rsp+24] RSP   = 0x%016lx", (unsigned long)stack_ptr[3]);
    log_info("arch", "  [rsp+32] SS    = 0x%016lx", (unsigned long)stack_ptr[4]);
}
