#include <tnu/console.h>
#include <tnu/printf.h>
#include <tnu/string.h>

static void out_char(char **buf, size_t *remaining, int *written, char c)
{
    if (*remaining > 1) {
        **buf = c;
        (*buf)++;
        (*remaining)--;
    }
    (*written)++;
}

static void out_string(char **buf, size_t *remaining, int *written, const char *s)
{
    if (!s) {
        s = "(null)";
    }
    while (*s) {
        out_char(buf, remaining, written, *s++);
    }
}

static int unsigned_to_string(char *out, size_t out_size, uint64_t value,
                              unsigned base, bool upper)
{
    char tmp[32];
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    size_t i = 0;

    if (value == 0) {
        if (out_size > 1) {
            out[0] = '0';
            out[1] = '\0';
        }
        return 1;
    }
    while (value && i < sizeof(tmp)) {
        tmp[i++] = digits[value % base];
        value /= base;
    }
    size_t len = i;
    if (out_size) {
        size_t j = 0;
        while (i && j + 1 < out_size) {
            out[j++] = tmp[--i];
        }
        out[j] = '\0';
    }
    return (int)len;
}

static void out_padded(char **buf, size_t *remaining, int *written,
                       const char *s, int width, bool left, char pad)
{
    if (!s) {
        s = "(null)";
    }
    int len = (int)strlen(s);
    if (!left && pad == '0' && s[0] == '-') {
        out_char(buf, remaining, written, '-');
        s++;
        len--;
        width--;
    }
    if (!left) {
        while (width > len) {
            out_char(buf, remaining, written, pad);
            width--;
        }
    }
    while (*s) {
        out_char(buf, remaining, written, *s++);
    }
    if (left) {
        while (width > len) {
            out_char(buf, remaining, written, ' ');
            width--;
        }
    }
}

static void out_unsigned(char **buf, size_t *remaining, int *written,
                         uint64_t value, unsigned base, bool upper,
                         int width, bool left, char pad)
{
    char tmp[64];
    unsigned_to_string(tmp, sizeof(tmp), value, base, upper);
    out_padded(buf, remaining, written, tmp, width, left, pad);
}

static void out_signed(char **buf, size_t *remaining, int *written,
                       int64_t value, int width, bool left, char pad)
{
    char tmp[64];
    if (value < 0) {
        tmp[0] = '-';
        unsigned_to_string(tmp + 1, sizeof(tmp) - 1, (uint64_t)-value, 10, false);
    } else {
        unsigned_to_string(tmp, sizeof(tmp), (uint64_t)value, 10, false);
    }
    out_padded(buf, remaining, written, tmp, width, left, pad);
}

static int parse_width(const char **fmt)
{
    int width = 0;
    while (**fmt >= '0' && **fmt <= '9') {
        width = width * 10 + (**fmt - '0');
        (*fmt)++;
    }
    return width;
}

int kvsnprintf(char *buf, size_t size, const char *fmt, va_list ap)
{
    char *out = buf;
    size_t remaining = size;
    int written = 0;

    if (size == 0) {
        remaining = 0;
    }

    for (; *fmt; fmt++) {
        if (*fmt != '%') {
            out_char(&out, &remaining, &written, *fmt);
            continue;
        }

        fmt++;
        bool left = false;
        char pad = ' ';
        if (*fmt == '-') {
            left = true;
            fmt++;
        }
        if (*fmt == '0') {
            pad = '0';
            fmt++;
        }
        int width = parse_width(&fmt);
        bool long_value = false;
        if (*fmt == 'l') {
            long_value = true;
            fmt++;
            if (*fmt == 'l') {
                fmt++;
            }
        } else if (*fmt == 'z') {
            long_value = true;
            fmt++;
        }

        switch (*fmt) {
        case '%':
            out_char(&out, &remaining, &written, '%');
            break;
        case 'c':
        {
            char s[2] = { (char)va_arg(ap, int), '\0' };
            out_padded(&out, &remaining, &written, s, width, left, pad);
            break;
        }
        case 's':
            if (width > 0) {
                out_padded(&out, &remaining, &written, va_arg(ap, const char *), width, left, pad);
            } else {
                out_string(&out, &remaining, &written, va_arg(ap, const char *));
            }
            break;
        case 'd':
        case 'i': {
            int64_t v = long_value ? va_arg(ap, int64_t) : va_arg(ap, int);
            out_signed(&out, &remaining, &written, v, width, left, pad);
            break;
        }
        case 'u': {
            uint64_t v = long_value ? va_arg(ap, uint64_t) : va_arg(ap, unsigned int);
            out_unsigned(&out, &remaining, &written, v, 10, false, width, left, pad);
            break;
        }
        case 'o': {
            uint64_t v = long_value ? va_arg(ap, uint64_t) : va_arg(ap, unsigned int);
            out_unsigned(&out, &remaining, &written, v, 8, false, width, left, pad);
            break;
        }
        case 'x':
        case 'X': {
            uint64_t v = long_value ? va_arg(ap, uint64_t) : va_arg(ap, unsigned int);
            out_unsigned(&out, &remaining, &written, v, 16, *fmt == 'X', width, left, pad);
            break;
        }
        case 'p':
            out_string(&out, &remaining, &written, "0x");
            out_unsigned(&out, &remaining, &written, (uintptr_t)va_arg(ap, void *), 16, false,
                         width, left, pad);
            break;
        default:
            out_char(&out, &remaining, &written, '%');
            out_char(&out, &remaining, &written, *fmt);
            break;
        }
    }

    if (size > 0) {
        *out = '\0';
    }
    return written;
}

int ksnprintf(char *buf, size_t size, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int ret = kvsnprintf(buf, size, fmt, ap);
    va_end(ap);
    return ret;
}

int kprintf(const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    int ret = kvsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    console_write(buf);
    return ret;
}
