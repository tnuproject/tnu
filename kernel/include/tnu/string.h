#ifndef TNU_STRING_H
#define TNU_STRING_H

#include <tnu/types.h>

void *memset(void *dest, int value, size_t n);
/* Fill 'count' uint32_t words starting at 'dest' with 'value'.
 * Faster than memset for framebuffer pixel fills: uses 'rep stosl'. */
void *memset32(void *dest, uint32_t value, size_t count);
void *memcpy(void *dest, const void *src, size_t n);
void *memmove(void *dest, const void *src, size_t n);
int memcmp(const void *a, const void *b, size_t n);
size_t strlen(const char *s);
size_t strnlen(const char *s, size_t max);
int strcmp(const char *a, const char *b);
int strncmp(const char *a, const char *b, size_t n);
char *strcpy(char *dest, const char *src);
char *strncpy(char *dest, const char *src, size_t n);
char *strcat(char *dest, const char *src);
char *strchr(const char *s, int c);
char *strrchr(const char *s, int c);
char *strstr(const char *haystack, const char *needle);
long strtol(const char *s, char **end, int base);
int atoi(const char *s);

#endif
