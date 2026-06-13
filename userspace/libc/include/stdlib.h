#ifndef TNU_STDLIB_H
#define TNU_STDLIB_H

#include <stddef.h>
#include <sys/types.h>

void qsort(void *base, size_t nmemb, size_t size,
           int (*compar)(const void *, const void *));
void *bsearch(const void *key, const void *base, size_t nmemb, size_t size,
              int (*compar)(const void *, const void *));
long long atoll(const char *s);
char *getenv(const char *name);
int setenv(const char *name, const char *value, int overwrite);
int unsetenv(const char *name);
int atoi(const char *s);
double atof(const char *s);
int abs(int value);
long strtol(const char *nptr, char **endptr, int base);
unsigned long strtoul(const char *nptr, char **endptr, int base);
char *realpath(const char *path, char *resolved_path);
int mkstemps(char *template_name, int suffixlen);
int mkstemp(char *template_name);
void *malloc(size_t size);
void *calloc(size_t nmemb, size_t size);
void *realloc(void *ptr, size_t size);
void free(void *ptr);
int system(const char *command);
int putenv(char *string);
void exit(int code) __attribute__((noreturn));

#endif
