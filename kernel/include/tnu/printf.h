#ifndef TNU_PRINTF_H
#define TNU_PRINTF_H

#include <stdarg.h>
#include <tnu/types.h>

int kvsnprintf(char *buf, size_t size, const char *fmt, va_list ap);
int ksnprintf(char *buf, size_t size, const char *fmt, ...);
int kprintf(const char *fmt, ...);

#endif
