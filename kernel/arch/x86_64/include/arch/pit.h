#ifndef TNU_ARCH_PIT_H
#define TNU_ARCH_PIT_H

#include <tnu/types.h>

/* PIT frequency — must match the value passed to pit_init() */
#define PIT_HZ 100u

void pit_init(uint32_t frequency);
void pit_tick(void);
uint64_t pit_ticks(void);
uint64_t pit_uptime_seconds(void);

/* Convenience alias used by syscall.c */
static inline uint64_t pit_get_ticks(void) { return pit_ticks(); }

/* Millisecond uptime using PIT ticks */
static inline uint64_t pit_uptime_ms(void) { return pit_ticks() * (1000u / PIT_HZ); }

#endif
