#ifndef TNU_ARCH_CPU_H
#define TNU_ARCH_CPU_H

#include <tnu/types.h>

static inline void cpu_halt(void)
{
    __asm__ volatile("hlt");
}

static inline void cpu_pause(void)
{
    __asm__ volatile("pause");
}

static inline void cpu_cli(void)
{
    __asm__ volatile("cli");
}

static inline void cpu_sti(void)
{
    __asm__ volatile("sti");
}

void cpu_get_brand(char *out, size_t out_size);
uint64_t cpu_read_cr2(void);
void cpu_init_fpu(void);
void cpu_set_fs_base(uint64_t base);
uint64_t cpu_get_fs_base(void);
void syscall_init(void);
int arch_enter_user(uint64_t entry, uint64_t user_stack);
__attribute__((noreturn)) void arch_abort_user(int exit_code);

#endif
