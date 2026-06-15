#ifndef TNU_IRQ_H
#define TNU_IRQ_H

#include <tnu/types.h>

typedef void (*irq_handler_t)(int irq, void *arg);

static inline int irq_register(int irq, irq_handler_t handler, const char *name)
{
    (void)irq;
    (void)handler;
    (void)name;
    return 0;
}

static inline void irq_unregister(int irq)
{
    (void)irq;
}

static inline void irq_eoi(int irq)
{
    (void)irq;
}

#endif
