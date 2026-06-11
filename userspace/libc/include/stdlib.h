#ifndef TNU_STDLIB_H
#define TNU_STDLIB_H

#include <stddef.h>
#include <sys/types.h>

int atoi(const char *s);
long strtol(const char *nptr, char **endptr, int base);
void *malloc(size_t size);
void *calloc(size_t nmemb, size_t size);
void *realloc(void *ptr, size_t size);
void free(void *ptr);
void exit(int code) __attribute__((noreturn));

#endif
