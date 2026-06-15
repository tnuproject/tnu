#include <tnu/string.h>

/*
 * Fast memset using 64-bit stores via 'rep stosq'.
 * Handles any size and alignment:
 *   1. Byte-fill the unaligned head (0–7 bytes) to reach 8-byte alignment.
 *   2. Use 'rep stosq' for the bulk (8 bytes per iteration).
 *   3. Byte-fill the remaining tail.
 */
void *memset(void *dest, int value, size_t n)
{
    unsigned char *d = (unsigned char *)dest;
    unsigned char v = (unsigned char)value;

    /* Head: align to 8 bytes */
    while (n > 0 && ((uintptr_t)d & 7)) {
        *d++ = v;
        n--;
    }

    /* Bulk: 8 bytes at a time with rep stosq */
    if (n >= 8) {
        uint64_t v64 = (uint64_t)v * 0x0101010101010101ULL;
        size_t qwords = n >> 3;
        __asm__ volatile(
            "rep stosq"
            : "+D"(d), "+c"(qwords)
            : "a"(v64)
            : "memory"
        );
        n &= 7;
    }

    /* Tail */
    while (n--) {
        *d++ = v;
    }

    return dest;
}

/*
 * memset32 — fill memory with a repeating 32-bit word (e.g. a pixel colour).
 * Uses 'rep stosd' which is ideal for framebuffer fills.
 * dest must be 4-byte aligned; count is the number of uint32_t words to write.
 */
void *memset32(void *dest, uint32_t value, size_t count)
{
    uint32_t *d = (uint32_t *)dest;
    __asm__ volatile(
        "rep stosl"
        : "+D"(d), "+c"(count)
        : "a"(value)
        : "memory"
    );
    return dest;
}

/*
 * Fast memcpy using 64-bit loads/stores via 'rep movsq'.
 * Handles any size and alignment:
 *   1. Byte-copy the unaligned head to reach 8-byte alignment.
 *   2. Use 'rep movsq' for the bulk.
 *   3. Byte-copy the remaining tail.
 */
void *memcpy(void *dest, const void *src, size_t n)
{
    unsigned char *d = (unsigned char *)dest;
    const unsigned char *s = (const unsigned char *)src;

    /* Head: align destination to 8 bytes */
    while (n > 0 && ((uintptr_t)d & 7)) {
        *d++ = *s++;
        n--;
    }

    /* Bulk: 8 bytes at a time with rep movsq */
    if (n >= 8) {
        size_t qwords = n >> 3;
        __asm__ volatile(
            "rep movsq"
            : "+D"(d), "+S"(s), "+c"(qwords)
            :
            : "memory"
        );
        n &= 7;
    }

    /* Tail */
    while (n--) {
        *d++ = *s++;
    }

    return dest;
}

/*
 * Fast memmove using 'rep movsq' forward and 'std; rep movsb; cld' backward.
 * Forward path mirrors memcpy; backward path walks from the end to avoid
 * aliasing when dest > src (overlapping regions).
 */
void *memmove(void *dest, const void *src, size_t n)
{
    unsigned char *d = (unsigned char *)dest;
    const unsigned char *s = (const unsigned char *)src;

    if (d == s || n == 0) {
        return dest;
    }

    if (d < s) {
        /* Forward copy — same as memcpy fast path */
        while (n > 0 && ((uintptr_t)d & 7)) {
            *d++ = *s++;
            n--;
        }
        if (n >= 8) {
            size_t qwords = n >> 3;
            __asm__ volatile(
                "rep movsq"
                : "+D"(d), "+S"(s), "+c"(qwords)
                :
                : "memory"
            );
            n &= 7;
        }
        while (n--) {
            *d++ = *s++;
        }
    } else {
        /* Backward copy — walk from end to start */
        d += n;
        s += n;
        /* Byte-copy unaligned tail at the high end */
        while (n > 0 && ((uintptr_t)d & 7)) {
            *--d = *--s;
            n--;
        }
        /* Bulk: 8 bytes at a time descending.
         * With STD set, 'rep movsq' reads from [RSI] and writes to [RDI],
         * then decrements both by 8.  So we point RSI/RDI at the last
         * qword to copy (i.e. 8 bytes below the current top pointer). */
        if (n >= 8) {
            size_t qwords = n >> 3;
            d -= 8;   /* point at last destination qword */
            s -= 8;   /* point at last source qword      */
            __asm__ volatile(
                "std\n\t"
                "rep movsq\n\t"
                "cld"
                : "+D"(d), "+S"(s), "+c"(qwords)
                :
                : "memory"
            );
            /* After the loop d and s have been decremented qwords-1 more
             * times and each points 8 bytes below the start of the copied
             * region — adjust back to the true start for the tail. */
            d += 8;
            s += 8;
            n &= 7;
        }
        /* Remaining bytes */
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
