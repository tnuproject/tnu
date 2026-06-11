#include <tnu/libc.h>
#include <stdarg.h>

void print(const char *s)
{
    write(1, s, strlen(s));
}

void println(const char *s)
{
    print(s);
    write(1, "\n", 1);
}

void print_int(long value)
{
    char buf[32];
    int i = 0;
    if (value == 0) {
        write(1, "0", 1);
        return;
    }
    if (value < 0) {
        write(1, "-", 1);
        value = -value;
    }
    while (value && i < (int)sizeof(buf)) {
        buf[i++] = (char)('0' + (value % 10));
        value /= 10;
    }
    while (i--) {
        write(1, &buf[i], 1);
    }
}

void print_hex(uint64_t value)
{
    char buf[16];
    const char *digits = "0123456789abcdef";
    int i = 0;
    print("0x");
    if (value == 0) {
        write(1, "0", 1);
        return;
    }
    while (value && i < 16) {
        buf[i++] = digits[value & 0xf];
        value >>= 4;
    }
    while (i--) {
        write(1, &buf[i], 1);
    }
}

int putchar(int c)
{
    char ch = (char)c;
    return write(1, &ch, 1) == 1 ? (unsigned char)ch : -1;
}

int puts(const char *s)
{
    int n = 0;
    if (s) {
        size_t len = strlen(s);
        write(1, s, len);
        n += (int)len;
    }
    write(1, "\n", 1);
    return n + 1;
}

static int write_string(const char *s)
{
    if (!s) {
        s = "(null)";
    }
    size_t len = strlen(s);
    write(1, s, len);
    return (int)len;
}

static int write_unsigned(unsigned long value, unsigned base, int alt)
{
    char buf[32];
    const char *digits = "0123456789abcdef";
    int i = 0;
    int n = 0;
    if (alt && base == 16) {
        n += write_string("0x");
    }
    if (value == 0) {
        putchar('0');
        return n + 1;
    }
    while (value && i < (int)sizeof(buf)) {
        buf[i++] = digits[value % base];
        value /= base;
    }
    while (i--) {
        putchar(buf[i]);
        n++;
    }
    return n;
}

int vprintf(const char *fmt, va_list ap)
{
    int written = 0;
    for (const char *p = fmt; *p; p++) {
        if (*p != '%') {
            putchar(*p);
            written++;
            continue;
        }
        p++;
        if (*p == '%') {
            putchar('%');
            written++;
            continue;
        }
        int long_arg = 0;
        if (*p == 'l') {
            long_arg = 1;
            p++;
            if (*p == 'l') {
                long_arg = 2;
                p++;
            }
        }
        switch (*p) {
        case 'c':
            putchar(va_arg(ap, int));
            written++;
            break;
        case 's':
            written += write_string(va_arg(ap, const char *));
            break;
        case 'd':
        case 'i': {
            long value = long_arg ? va_arg(ap, long) : va_arg(ap, int);
            if (value < 0) {
                putchar('-');
                written++;
                value = -value;
            }
            written += write_unsigned((unsigned long)value, 10, 0);
            break;
        }
        case 'u': {
            unsigned long value = long_arg ? va_arg(ap, unsigned long) : va_arg(ap, unsigned int);
            written += write_unsigned(value, 10, 0);
            break;
        }
        case 'x': {
            unsigned long value = long_arg ? va_arg(ap, unsigned long) : va_arg(ap, unsigned int);
            written += write_unsigned(value, 16, 0);
            break;
        }
        case 'p':
            written += write_unsigned((uintptr_t)va_arg(ap, void *), 16, 1);
            break;
        default:
            putchar('%');
            putchar(*p);
            written += 2;
            break;
        }
    }
    return written;
}

int printf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int ret = vprintf(fmt, ap);
    va_end(ap);
    return ret;
}
