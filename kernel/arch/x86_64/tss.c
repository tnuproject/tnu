#include <arch/tss.h>
#include <tnu/types.h>
#include <tnu/string.h>

#define TSS_SELECTOR 0x28

struct tss64 {
    uint32_t reserved0;
    uint64_t rsp0;
    uint64_t rsp1;
    uint64_t rsp2;
    uint64_t reserved1;
    uint64_t ist1;
    uint64_t ist2;
    uint64_t ist3;
    uint64_t ist4;
    uint64_t ist5;
    uint64_t ist6;
    uint64_t ist7;
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iomap_base;
} __attribute__((packed));

struct gdt_ptr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

static struct tss64 tss __attribute__((aligned(16)));
static uint8_t emergency_stack[65536] __attribute__((aligned(16)));
static uint8_t ring0_stack[65536] __attribute__((aligned(16)));

static uint64_t gdt[7] __attribute__((aligned(16)));

static void set_tss_descriptor(uint32_t index, uint64_t base, uint32_t limit)
{
    uint64_t low = 0;
    uint64_t high = 0;

    low |= (limit & 0xffffULL);
    low |= (base & 0xffffffULL) << 16;
    low |= 0x89ULL << 40;
    low |= ((limit >> 16) & 0xfULL) << 48;
    low |= ((base >> 24) & 0xffULL) << 56;

    high = base >> 32;

    gdt[index] = low;
    gdt[index + 1] = high;
}

void tss_init(void)
{
    memset(&tss, 0, sizeof(tss));

    tss.rsp0 = (uint64_t)(ring0_stack + sizeof(ring0_stack));
    tss.ist1 = (uint64_t)(emergency_stack + sizeof(emergency_stack));
    tss.iomap_base = sizeof(tss);

    gdt[0] = 0x0000000000000000ULL;
    gdt[1] = 0x00af9a000000ffffULL;
    gdt[2] = 0x00af92000000ffffULL;
    gdt[3] = 0x00aff2000000ffffULL;
    gdt[4] = 0x00affa000000ffffULL;

    set_tss_descriptor(5, (uint64_t)&tss, sizeof(tss) - 1);

    struct gdt_ptr ptr = {
        .limit = sizeof(gdt) - 1,
        .base = (uint64_t)gdt,
    };

    __asm__ volatile("lgdt %0" : : "m"(ptr));
    __asm__ volatile(
        "pushq $0x08\n"
        "leaq 1f(%%rip), %%rax\n"
        "pushq %%rax\n"
        "lretq\n"
        "1:\n"
        "movw $0x10, %%ax\n"
        "movw %%ax, %%ds\n"
        "movw %%ax, %%es\n"
        "movw %%ax, %%ss\n"
        "movw %%ax, %%fs\n"
        "movw %%ax, %%gs\n"
        :
        :
        : "rax", "memory"
    );

    __asm__ volatile("ltr %%ax" : : "a"(TSS_SELECTOR));
}
