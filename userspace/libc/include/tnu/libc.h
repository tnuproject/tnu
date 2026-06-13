#ifndef TNU_USER_LIBC_H
#define TNU_USER_LIBC_H

#include <stddef.h>
#include <stdint.h>
#include <tnu/syscall.h>

size_t strlen(const char *s);
int strcmp(const char *a, const char *b);
int strncmp(const char *a, const char *b, size_t n);
char *strcpy(char *dest, const char *src);
char *strncpy(char *dest, const char *src, size_t n);
char *strcat(char *dest, const char *src);
char *strchr(const char *s, int c);
char *strrchr(const char *s, int c);
char *strpbrk(const char *s, const char *accept);
char *strstr(const char *haystack, const char *needle);
char *strcasestr(const char *haystack, const char *needle);
char *strdup(const char *s);
char *strerror(int errnum);
int strcasecmp(const char *a, const char *b);
int strncasecmp(const char *a, const char *b, size_t n);
void *memset(void *dest, int value, size_t n);
void *memcpy(void *dest, const void *src, size_t n);
void *memmove(void *dest, const void *src, size_t n);
int memcmp(const void *a, const void *b, size_t n);
int atoi(const char *s);
int abs(int value);
long strtol(const char *nptr, char **endptr, int base);
void *malloc(size_t size);
void *calloc(size_t nmemb, size_t size);
void *realloc(void *ptr, size_t size);
void free(void *ptr);
void print(const char *s);
void println(const char *s);
void print_int(long value);
void print_hex(uint64_t value);
int putchar(int c);
int puts(const char *s);
int printf(const char *fmt, ...);

#endif
