#include <arch/idt.h>
#include <arch/pic.h>
#include <tnu/string.h>

struct idt_entry {
    uint16_t offset_low;
    uint16_t selector;
    uint8_t ist;
    uint8_t type_attr;
    uint16_t offset_mid;
    uint32_t offset_high;
    uint32_t zero;
} __attribute__((packed));

struct idt_ptr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

static struct idt_entry idt[256];

#define DECL_ISR(n) extern void isr##n(void)
DECL_ISR(0); DECL_ISR(1); DECL_ISR(2); DECL_ISR(3); DECL_ISR(4); DECL_ISR(5);
DECL_ISR(6); DECL_ISR(7); DECL_ISR(8); DECL_ISR(9); DECL_ISR(10); DECL_ISR(11);
DECL_ISR(12); DECL_ISR(13); DECL_ISR(14); DECL_ISR(15); DECL_ISR(16); DECL_ISR(17);
DECL_ISR(18); DECL_ISR(19); DECL_ISR(20); DECL_ISR(21); DECL_ISR(22); DECL_ISR(23);
DECL_ISR(24); DECL_ISR(25); DECL_ISR(26); DECL_ISR(27); DECL_ISR(28); DECL_ISR(29);
DECL_ISR(30); DECL_ISR(31); DECL_ISR(32); DECL_ISR(33); DECL_ISR(34); DECL_ISR(35);
DECL_ISR(36); DECL_ISR(37); DECL_ISR(38); DECL_ISR(39); DECL_ISR(40); DECL_ISR(41);
DECL_ISR(42); DECL_ISR(43); DECL_ISR(44); DECL_ISR(45); DECL_ISR(46); DECL_ISR(47);

static void idt_set_gate(uint8_t vector, void (*handler)(void), uint8_t flags)
{
    uint64_t addr = (uint64_t)handler;
    idt[vector].offset_low = (uint16_t)(addr & 0xffff);
    idt[vector].selector = 0x08;
    if (vector == 8 || vector == 13 || vector == 14) {
        idt[vector].ist = 1;
    } else {
        idt[vector].ist = 0;
    }
    idt[vector].type_attr = flags;
    idt[vector].offset_mid = (uint16_t)((addr >> 16) & 0xffff);
    idt[vector].offset_high = (uint32_t)((addr >> 32) & 0xffffffff);
    idt[vector].zero = 0;
}

void idt_init(void)
{
    memset(idt, 0, sizeof(idt));
    void (*handlers[48])(void) = {
        isr0, isr1, isr2, isr3, isr4, isr5, isr6, isr7,
        isr8, isr9, isr10, isr11, isr12, isr13, isr14, isr15,
        isr16, isr17, isr18, isr19, isr20, isr21, isr22, isr23,
        isr24, isr25, isr26, isr27, isr28, isr29, isr30, isr31,
        isr32, isr33, isr34, isr35, isr36, isr37, isr38, isr39,
        isr40, isr41, isr42, isr43, isr44, isr45, isr46, isr47,
    };
    for (uint8_t i = 0; i < 48; i++) {
        idt_set_gate(i, handlers[i], 0x8e);
    }

    struct idt_ptr ptr = {
        .limit = sizeof(idt) - 1,
        .base = (uint64_t)idt,
    };
    __asm__ volatile("lidt %0" : : "m"(ptr));

    pic_remap(32, 40);
    for (uint8_t irq = 0; irq < 16; irq++) {
        pic_set_mask(irq);
    }
    pic_clear_mask(0);
    pic_clear_mask(1);
}
