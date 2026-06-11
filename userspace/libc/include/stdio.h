#ifndef TNU_STDIO_H
#define TNU_STDIO_H

#include <stdarg.h>
#include <stddef.h>

#define EOF (-1)

int putchar(int c);
int puts(const char *s);
int printf(const char *fmt, ...);
int vprintf(const char *fmt, va_list ap);

#endif
