#include <arch/cpu.h>
#include <tnu/string.h>

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
