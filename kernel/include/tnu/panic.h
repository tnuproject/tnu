#ifndef TNU_PANIC_H
#define TNU_PANIC_H

#include <tnu/types.h>

struct interrupt_frame;

__attribute__((noreturn)) void panic(const char *message);
__attribute__((noreturn)) void panic_exception(struct interrupt_frame *frame, const char *message);

#endif
