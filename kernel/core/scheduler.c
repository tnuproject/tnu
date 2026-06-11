#include <tnu/scheduler.h>

static volatile uint64_t ticks;

void scheduler_init(void)
{
    ticks = 0;
}

void scheduler_tick(void)
{
    ticks++;
}

uint64_t scheduler_ticks(void)
{
    return ticks;
}
