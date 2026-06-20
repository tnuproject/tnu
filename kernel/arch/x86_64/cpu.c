#include <arch/cpu.h>
#include <tnu/string.h>

extern void syscall_entry(void);

static void wrmsr(uint32_t msr, uint64_t value)
{
    uint32_t lo = (uint32_t)value;
    uint32_t hi = (uint32_t)(value >> 32);
    __asm__ volatile("wrmsr" : : "c"(msr), "a"(lo), "d"(hi));
}

static uint64_t rdmsr(uint32_t msr)
{
    uint32_t lo;
    uint32_t hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

static void cpuid(uint32_t leaf, uint32_t subleaf, uint32_t *a, uint32_t *b,
                  uint32_t *c, uint32_t *d)
{
    __asm__ volatile("cpuid"
                     : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d)
                     : "a"(leaf), "c"(subleaf));
}

void cpu_get_brand(char *out, size_t out_size)
{
    uint32_t max_ext, b, c, d;
    uint32_t regs[12];

    if (out_size == 0) {
        return;
    }
    out[0] = '\0';
    cpuid(0x80000000, 0, &max_ext, &b, &c, &d);
    if (max_ext < 0x80000004) {
        strncpy(out, "unknown x86_64 CPU", out_size - 1);
        out[out_size - 1] = '\0';
        return;
    }

    for (uint32_t i = 0; i < 3; i++) {
        cpuid(0x80000002 + i, 0, &regs[i * 4], &regs[i * 4 + 1],
              &regs[i * 4 + 2], &regs[i * 4 + 3]);
    }
    size_t n = out_size - 1;
    if (n > sizeof(regs)) {
        n = sizeof(regs);
    }
    memcpy(out, regs, n);
    out[n] = '\0';
}

uint64_t cpu_read_cr2(void)
{
    uint64_t value;
    __asm__ volatile("movq %%cr2, %0" : "=r"(value));
    return value;
}

uint64_t cpu_get_fs_base(void)
{
    return rdmsr(0xc0000100);
}

void cpu_set_fs_base(uint64_t base)
{
    wrmsr(0xc0000100, base);
}

void cpu_init_fpu(void)
{
    uint32_t a, b, c, d;
    cpuid(1, 0, &a, &b, &c, &d);

    enum {
        CPUID_EDX_FXSR = 1u << 24,
        CPUID_EDX_SSE = 1u << 25,
        CR0_MP = 1u << 1,
        CR0_EM = 1u << 2,
        CR0_TS = 1u << 3,
        CR4_OSFXSR = 1u << 9,
        CR4_OSXMMEXCPT = 1u << 10,
    };

    uint64_t cr0;
    __asm__ volatile("movq %%cr0, %0" : "=r"(cr0));
    cr0 |= CR0_MP;
    cr0 &= ~(uint64_t)(CR0_EM | CR0_TS);
    __asm__ volatile("movq %0, %%cr0" : : "r"(cr0) : "memory");

    if ((d & (CPUID_EDX_FXSR | CPUID_EDX_SSE)) == (CPUID_EDX_FXSR | CPUID_EDX_SSE)) {
        uint64_t cr4;
        __asm__ volatile("movq %%cr4, %0" : "=r"(cr4));
        cr4 |= CR4_OSFXSR | CR4_OSXMMEXCPT;
        __asm__ volatile("movq %0, %%cr4" : : "r"(cr4) : "memory");
    }

    __asm__ volatile("fninit");
}

void syscall_init(void)
{
    enum {
        IA32_EFER = 0xc0000080,
        IA32_STAR = 0xc0000081,
        IA32_LSTAR = 0xc0000082,
        IA32_FMASK = 0xc0000084,
        EFER_SCE = 1,
    };

    wrmsr(IA32_EFER, rdmsr(IA32_EFER) | EFER_SCE);
    /* STAR[47:32] = SYSCALL CS → CS=0x08, SS=0x08+8=0x10
     * STAR[63:48] = SYSRET base → 64-bit SYSRET CS=base+16=0x23, SS=base+8=0x1b */
    wrmsr(IA32_STAR, ((uint64_t)0x10 << 48) | ((uint64_t)0x08 << 32));
    wrmsr(IA32_LSTAR, (uint64_t)(uintptr_t)syscall_entry);
    wrmsr(IA32_FMASK, 0x200);
}
