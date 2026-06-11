#ifndef TNU_SCHEDULER_H
#define TNU_SCHEDULER_H

#include <tnu/types.h>

void scheduler_init(void);
void scheduler_tick(void);
uint64_t scheduler_ticks(void);

#endif
