#include <tnu/libc.h>

size_t strlen(const char *s)
{
    size_t n = 0;
    while (s && s[n]) {
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
    return n ? (unsigned char)*a - (unsigned char)*b : 0;
}

char *strcpy(char *dest, const char *src)
{
    char *d = dest;
    while ((*d++ = *src++) != 0) {
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
        dest[i] = 0;
    }
    return dest;
}

char *strcat(char *dest, const char *src)
{
    char *d = dest + strlen(dest);
    while ((*d++ = *src++) != 0) {
    }
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
    return c == 0 ? (char *)s : 0;
}

char *strrchr(const char *s, int c)
{
    const char *last = 0;
    do {
        if (*s == (char)c) {
            last = s;
        }
    } while (*s++);
    return (char *)last;
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
    return 0;
}

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
    if (d == s || n == 0) {
        return dest;
    }
    if (d < s) {
        while (n--) {
            *d++ = *s++;
        }
    } else {
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
    while (n--) {
        if (*pa != *pb) {
            return *pa - *pb;
        }
        pa++;
        pb++;
    }
    return 0;
}

int atoi(const char *s)
{
    int sign = 1;
    int value = 0;
    if (*s == '-') {
        sign = -1;
        s++;
    }
    while (*s >= '0' && *s <= '9') {
        value = value * 10 + (*s - '0');
        s++;
    }
    return value * sign;
}

long strtol(const char *nptr, char **endptr, int base)
{
    const char *p = nptr;
    long sign = 1;
    long value = 0;

    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') {
        p++;
    }
    if (*p == '-' || *p == '+') {
        sign = *p == '-' ? -1 : 1;
        p++;
    }
    if ((base == 0 || base == 16) && p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
        base = 16;
        p += 2;
    } else if (base == 0 && *p == '0') {
        base = 8;
        p++;
    } else if (base == 0) {
        base = 10;
    }

    while (*p) {
        int digit;
        if (*p >= '0' && *p <= '9') {
            digit = *p - '0';
        } else if (*p >= 'a' && *p <= 'z') {
            digit = *p - 'a' + 10;
        } else if (*p >= 'A' && *p <= 'Z') {
            digit = *p - 'A' + 10;
        } else {
            break;
        }
        if (digit >= base) {
            break;
        }
        value = value * base + digit;
        p++;
    }
    if (endptr) {
        *endptr = (char *)p;
    }
    return value * sign;
}
