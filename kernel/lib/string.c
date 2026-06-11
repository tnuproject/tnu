#include <tnu/string.h>

void *memset(void *dest, int value, size_t n)
{
    unsigned char *d = dest;
    while (n--) {
        *d++ = (unsigned char)value;
    }
    return dest;
}

void *memcpy(void *dest, const void *src, size_t n)
{
    unsigned char *d = dest;
    const unsigned char *s = src;
    while (n--) {
        *d++ = *s++;
    }
    return dest;
}

void *memmove(void *dest, const void *src, size_t n)
{
    unsigned char *d = dest;
    const unsigned char *s = src;
    if (d < s) {
        while (n--) {
            *d++ = *s++;
        }
    } else if (d > s) {
        d += n;
        s += n;
        while (n--) {
            *--d = *--s;
        }
    }
    return dest;
}

int memcmp(const void *a, const void *b, size_t n)
{
    const unsigned char *pa = a;
    const unsigned char *pb = b;
    for (size_t i = 0; i < n; i++) {
        if (pa[i] != pb[i]) {
            return (int)pa[i] - (int)pb[i];
        }
    }
    return 0;
}

size_t strlen(const char *s)
{
    size_t n = 0;
    while (s && s[n]) {
        n++;
    }
    return n;
}

size_t strnlen(const char *s, size_t max)
{
    size_t n = 0;
    while (s && n < max && s[n]) {
        n++;
    }
    return n;
}

int strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

int strncmp(const char *a, const char *b, size_t n)
{
    while (n && *a && *a == *b) {
        a++;
        b++;
        n--;
    }
    if (n == 0) {
        return 0;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

char *strcpy(char *dest, const char *src)
{
    char *d = dest;
    while ((*d++ = *src++) != '\0') {
    }
    return dest;
}

char *strncpy(char *dest, const char *src, size_t n)
{
    size_t i = 0;
    for (; i < n && src[i]; i++) {
        dest[i] = src[i];
    }
    for (; i < n; i++) {
        dest[i] = '\0';
    }
    return dest;
}

char *strcat(char *dest, const char *src)
{
    strcpy(dest + strlen(dest), src);
    return dest;
}

char *strchr(const char *s, int c)
{
    while (*s) {
        if (*s == (char)c) {
            return (char *)s;
        }
        s++;
    }
    return c == 0 ? (char *)s : NULL;
}

char *strrchr(const char *s, int c)
{
    const char *last = NULL;
    while (*s) {
        if (*s == (char)c) {
            last = s;
        }
        s++;
    }
    return c == 0 ? (char *)s : (char *)last;
}

char *strstr(const char *haystack, const char *needle)
{
    if (!*needle) {
        return (char *)haystack;
    }
    for (; *haystack; haystack++) {
        const char *h = haystack;
        const char *n = needle;
        while (*h && *n && *h == *n) {
            h++;
            n++;
        }
        if (!*n) {
            return (char *)haystack;
        }
    }
    return NULL;
}

long strtol(const char *s, char **end, int base)
{
    long sign = 1;
    long value = 0;

    while (*s == ' ' || *s == '\t' || *s == '\n') {
        s++;
    }
    if (*s == '-') {
        sign = -1;
        s++;
    } else if (*s == '+') {
        s++;
    }
    if ((base == 0 || base == 16) && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        base = 16;
        s += 2;
    }
    if (base == 0) {
        base = s[0] == '0' ? 8 : 10;
    }

    while (*s) {
        int digit;
        if (*s >= '0' && *s <= '9') {
            digit = *s - '0';
        } else if (*s >= 'a' && *s <= 'f') {
            digit = *s - 'a' + 10;
        } else if (*s >= 'A' && *s <= 'F') {
            digit = *s - 'A' + 10;
        } else {
            break;
        }
        if (digit >= base) {
            break;
        }
        value = value * base + digit;
        s++;
    }
    if (end) {
        *end = (char *)s;
    }
    return value * sign;
}

int atoi(const char *s)
{
    return (int)strtol(s, NULL, 10);
}
