#include <arch/io.h>
#include <arch/pit.h>

static volatile uint64_t ticks;
static uint32_t hz = 100;

void pit_init(uint32_t frequency)
{
    hz = frequency ? frequency : 100;
    uint32_t divisor = 1193182 / hz;
    outb(0x43, 0x36);
    outb(0x40, (uint8_t)(divisor & 0xff));
    outb(0x40, (uint8_t)((divisor >> 8) & 0xff));
}

void pit_tick(void)
{
    ticks++;
}

uint64_t pit_ticks(void)
{
    return ticks;
}

uint64_t pit_uptime_seconds(void)
{
    return ticks / hz;
}
