#ifndef TNU_ARCH_PIT_H
#define TNU_ARCH_PIT_H

#include <tnu/types.h>

void pit_init(uint32_t frequency);
void pit_tick(void);
uint64_t pit_ticks(void);
uint64_t pit_uptime_seconds(void);

#endif
