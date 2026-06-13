#include <tnu/libc.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

struct FILE {
    int fd;
    int standard;
};

static FILE stdin_file = { 0, 1 };
static FILE stdout_file = { 1, 1 };
static FILE stderr_file = { 2, 1 };

FILE *stdin = &stdin_file;
FILE *stdout = &stdout_file;
FILE *stderr = &stderr_file;

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

int getc(FILE *stream)
{
    unsigned char ch;
    if (!stream || read(stream->fd, &ch, 1) != 1) {
        return EOF;
    }
    return ch;
}

int getchar(void)
{
    return getc(stdin);
}

int putc(int c, FILE *stream)
{
    unsigned char ch = (unsigned char)c;
    if (!stream || write(stream->fd, &ch, 1) != 1) {
        return EOF;
    }
    return ch;
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

int fputs(const char *s, FILE *stream)
{
    if (!s || !stream) {
        return EOF;
    }
    size_t len = strlen(s);
    return write(stream->fd, s, len) == (ssize_t)len ? (int)len : EOF;
}

int vprintf(const char *fmt, va_list ap)
{
    return vfprintf(stdout, fmt, ap);
}

struct format_out {
    char *buf;
    size_t size;
    size_t pos;
    FILE *stream;
};

static void emit_char(struct format_out *out, char c)
{
    if (out->buf && out->size) {
        if (out->pos + 1 < out->size) {
            out->buf[out->pos] = c;
        }
    } else if (out->stream) {
        write(out->stream->fd, &c, 1);
    }
    out->pos++;
}

static void emit_string(struct format_out *out, const char *s)
{
    if (!s) {
        s = "(null)";
    }
    while (*s) {
        emit_char(out, *s++);
    }
}

static void emit_string_n(struct format_out *out, const char *s, int precision)
{
    if (!s) {
        s = "(null)";
    }
    int n = 0;
    while (*s && (precision < 0 || n < precision)) {
        emit_char(out, *s++);
        n++;
    }
}

static void emit_unsigned_width(struct format_out *out, unsigned long long value,
                                unsigned base, int alt, int width,
                                int precision, int zero_pad)
{
    char buf[32];
    const char *digits = "0123456789abcdef";
    int i = 0;
    if (alt && base == 16) {
        emit_string(out, "0x");
    }
    if (value == 0) {
        buf[i++] = '0';
    } else {
        while (value && i < (int)sizeof(buf)) {
            buf[i++] = digits[value % base];
            value /= base;
        }
    }

    int digits_len = i;
    int min_digits = precision >= 0 ? precision : (zero_pad ? width : 0);
    while (digits_len < min_digits) {
        emit_char(out, '0');
        digits_len++;
    }
    while (i--) {
        emit_char(out, buf[i]);
    }
}

static int format_to(struct format_out *out, const char *fmt, va_list *ap)
{
    for (const char *p = fmt; *p; p++) {
        if (*p != '%') {
            emit_char(out, *p);
            continue;
        }
        p++;
        if (*p == '%') {
            emit_char(out, '%');
            continue;
        }
        int alt = 0;
        int zero_pad = 0;
        while (*p == ' ' || *p == '#' || *p == '+' || *p == '-' || *p == '0') {
            if (*p == '#') {
                alt = 1;
            } else if (*p == '0') {
                zero_pad = 1;
            }
            p++;
        }
        int width = 0;
        if (*p == '*') {
            width = va_arg(*ap, int);
            p++;
        }
        while (*p >= '0' && *p <= '9') {
            width = width * 10 + (*p - '0');
            p++;
        }
        int precision = -1;
        if (*p == '.') {
            p++;
            precision = 0;
            if (*p == '*') {
                precision = va_arg(*ap, int);
                p++;
            }
            while (*p >= '0' && *p <= '9') {
                precision = precision * 10 + (*p - '0');
                p++;
            }
        }
        enum {
            LEN_INT,
            LEN_LONG,
            LEN_LONGLONG,
            LEN_SIZE,
        } length = LEN_INT;
        if (*p == 'l') {
            length = LEN_LONG;
            p++;
            if (*p == 'l') {
                length = LEN_LONGLONG;
                p++;
            }
        } else if (*p == 'z') {
            length = LEN_SIZE;
            p++;
        }
        switch (*p) {
        case 'c':
            emit_char(out, (char)va_arg(*ap, int));
            break;
        case 's':
            emit_string_n(out, va_arg(*ap, const char *), precision);
            break;
        case 'd':
        case 'i': {
            long long value;
            if (length == LEN_LONGLONG) {
                value = va_arg(*ap, long long);
            } else if (length == LEN_LONG) {
                value = va_arg(*ap, long);
            } else if (length == LEN_SIZE) {
                value = (long long)va_arg(*ap, ssize_t);
            } else {
                value = va_arg(*ap, int);
            }
            if (value < 0) {
                emit_char(out, '-');
                value = -value;
            }
            emit_unsigned_width(out, (unsigned long long)value, 10, 0,
                                width, precision, zero_pad);
            break;
        }
        case 'u': {
            unsigned long long value;
            if (length == LEN_LONGLONG) {
                value = va_arg(*ap, unsigned long long);
            } else if (length == LEN_LONG) {
                value = va_arg(*ap, unsigned long);
            } else if (length == LEN_SIZE) {
                value = va_arg(*ap, size_t);
            } else {
                value = va_arg(*ap, unsigned int);
            }
            emit_unsigned_width(out, value, 10, 0, width, precision, zero_pad);
            break;
        }
        case 'o': {
            unsigned long long value;
            if (length == LEN_LONGLONG) {
                value = va_arg(*ap, unsigned long long);
            } else if (length == LEN_LONG) {
                value = va_arg(*ap, unsigned long);
            } else if (length == LEN_SIZE) {
                value = va_arg(*ap, size_t);
            } else {
                value = va_arg(*ap, unsigned int);
            }
            emit_unsigned_width(out, value, 8, alt, width, precision, zero_pad);
            break;
        }
        case 'x': {
            unsigned long long value;
            if (length == LEN_LONGLONG) {
                value = va_arg(*ap, unsigned long long);
            } else if (length == LEN_LONG) {
                value = va_arg(*ap, unsigned long);
            } else if (length == LEN_SIZE) {
                value = va_arg(*ap, size_t);
            } else {
                value = va_arg(*ap, unsigned int);
            }
            emit_unsigned_width(out, value, 16, alt, width, precision, zero_pad);
            break;
        }
        case 'X': {
            unsigned long long value;
            if (length == LEN_LONGLONG) {
                value = va_arg(*ap, unsigned long long);
            } else if (length == LEN_LONG) {
                value = va_arg(*ap, unsigned long);
            } else if (length == LEN_SIZE) {
                value = va_arg(*ap, size_t);
            } else {
                value = va_arg(*ap, unsigned int);
            }
            emit_unsigned_width(out, value, 16, alt, width, precision, zero_pad);
            break;
        }
        case 'p':
            emit_unsigned_width(out, (uintptr_t)va_arg(*ap, void *), 16, 1,
                                width, precision, zero_pad);
            break;
        case 'f':
            (void)va_arg(*ap, double);
            emit_string(out, "0.000000");
            break;
        default:
            emit_char(out, '%');
            emit_char(out, *p);
            break;
        }
    }
    if (out->buf && out->size) {
        size_t term = out->pos < out->size ? out->pos : out->size - 1;
        out->buf[term] = '\0';
    }
    return (int)out->pos;
}

int printf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    struct format_out out = { .stream = stdout };
    int ret = format_to(&out, fmt, &ap);
    va_end(ap);
    return ret;
}

int vfprintf(FILE *stream, const char *fmt, va_list ap)
{
    struct format_out out = { .stream = stream ? stream : stdout };
    va_list copy;
    va_copy(copy, ap);
    int ret = format_to(&out, fmt, &copy);
    va_end(copy);
    return ret;
}

int fprintf(FILE *stream, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    struct format_out out = { .stream = stream ? stream : stdout };
    int ret = format_to(&out, fmt, &ap);
    va_end(ap);
    return ret;
}

int vsnprintf(char *str, size_t size, const char *fmt, va_list ap)
{
    struct format_out out = { .buf = str, .size = size };
    va_list copy;
    va_copy(copy, ap);
    int ret = format_to(&out, fmt, &copy);
    va_end(copy);
    return ret;
}

int snprintf(char *str, size_t size, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    struct format_out out = { .buf = str, .size = size };
    int ret = format_to(&out, fmt, &ap);
    va_end(ap);
    return ret;
}

int vsprintf(char *str, const char *fmt, va_list ap)
{
    return vsnprintf(str, (size_t)-1, fmt, ap);
}

int sprintf(char *str, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    struct format_out out = { .buf = str, .size = (size_t)-1 };
    int ret = format_to(&out, fmt, &ap);
    va_end(ap);
    return ret;
}

ssize_t getline(char **lineptr, size_t *n, FILE *stream)
{
    if (!lineptr || !n || !stream) {
        errno = EINVAL;
        return -1;
    }
    if (!*lineptr || *n == 0) {
        *n = 128;
        *lineptr = malloc(*n);
        if (!*lineptr) {
            errno = ENOMEM;
            return -1;
        }
    }
    size_t len = 0;
    for (;;) {
        char ch;
        ssize_t got = read(stream->fd, &ch, 1);
        if (got <= 0) {
            return len ? (ssize_t)len : -1;
        }
        if (len + 1 >= *n) {
            size_t next_size = *n * 2;
            char *next = realloc(*lineptr, next_size);
            if (!next) {
                errno = ENOMEM;
                return -1;
            }
            *lineptr = next;
            *n = next_size;
        }
        (*lineptr)[len++] = ch;
        if (ch == '\n') {
            break;
        }
    }
    (*lineptr)[len] = '\0';
    return (ssize_t)len;
}

static int scan_is_space(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

static int scan_digit_value(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

static int scan_unsigned(const char **input, int base, int width, unsigned long *out)
{
    const char *p = *input;
    unsigned long value = 0;
    int consumed = 0;
    if (width <= 0) {
        width = 1024;
    }
    if ((base == 0 || base == 16) && width >= 2 && p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
        base = 16;
        p += 2;
        width -= 2;
        consumed += 2;
    } else if (base == 0 && *p == '0') {
        base = 8;
    } else if (base == 0) {
        base = 10;
    }
    while (*p && width > 0) {
        int digit = scan_digit_value(*p);
        if (digit < 0 || digit >= base) {
            break;
        }
        value = value * (unsigned long)base + (unsigned long)digit;
        p++;
        width--;
        consumed++;
    }
    if (consumed == 0 || (consumed == 2 && base == 16 && value == 0 && p[-1] == 'x')) {
        return 0;
    }
    *input = p;
    *out = value;
    return 1;
}

int sscanf(const char *str, const char *fmt, ...)
{
    va_list ap;
    int assigned = 0;
    const char *in = str;
    va_start(ap, fmt);

    while (*fmt) {
        if (scan_is_space(*fmt)) {
            while (scan_is_space(*fmt)) {
                fmt++;
            }
            while (scan_is_space(*in)) {
                in++;
            }
            continue;
        }
        if (*fmt != '%') {
            if (*in != *fmt) {
                break;
            }
            in++;
            fmt++;
            continue;
        }

        fmt++;
        int width = 0;
        while (*fmt >= '0' && *fmt <= '9') {
            width = width * 10 + (*fmt - '0');
            fmt++;
        }
        int half = 0;
        if (*fmt == 'h') {
            half = 1;
            fmt++;
        }

        while (scan_is_space(*in)) {
            in++;
        }

        if (*fmt == 'd' || *fmt == 'i' || *fmt == 'u' || *fmt == 'x' || *fmt == 'X' || *fmt == 'o') {
            int negative = 0;
            int base = *fmt == 'x' || *fmt == 'X' ? 16 : (*fmt == 'o' ? 8 : (*fmt == 'i' ? 0 : 10));
            if ((*fmt == 'd' || *fmt == 'i') && (*in == '-' || *in == '+')) {
                negative = *in == '-';
                in++;
                if (width > 0) {
                    width--;
                }
            }
            unsigned long value = 0;
            if (!scan_unsigned(&in, base, width, &value)) {
                break;
            }
            if (half) {
                unsigned short *dst = va_arg(ap, unsigned short *);
                *dst = (unsigned short)(negative ? -(long)value : (long)value);
            } else {
                int *dst = va_arg(ap, int *);
                *dst = (int)(negative ? -(long)value : (long)value);
            }
            assigned++;
        } else if (*fmt == 'c') {
            char *dst = va_arg(ap, char *);
            if (!*in) {
                break;
            }
            *dst = *in++;
            assigned++;
        } else if (*fmt == '%') {
            if (*in != '%') {
                break;
            }
            in++;
        } else {
            break;
        }
        fmt++;
    }

    va_end(ap);
    return assigned;
}

FILE *fopen(const char *path, const char *mode)
{
    int flags = O_RDONLY;
    if (mode && mode[0] == 'r' && strchr(mode, '+')) {
        flags = O_RDWR;
    } else if (mode && mode[0] == 'w' && strchr(mode, '+')) {
        flags = O_CREAT | O_TRUNC | O_RDWR;
    } else if (mode && mode[0] == 'a' && strchr(mode, '+')) {
        flags = O_CREAT | O_APPEND | O_RDWR;
    } else if (mode && mode[0] == 'w') {
        flags = O_CREAT | O_TRUNC | O_WRONLY;
    } else if (mode && mode[0] == 'a') {
        flags = O_CREAT | O_APPEND | O_WRONLY;
    }
    int fd = open(path, flags, 0644);
    if (fd < 0) {
        return 0;
    }
    FILE *f = malloc(sizeof(*f));
    if (!f) {
        close(fd);
        return 0;
    }
    f->fd = fd;
    f->standard = 0;
    if (mode && mode[0] == 'a') {
        lseek(fd, 0, SEEK_END);
    }
    return f;
}

FILE *fdopen(int fd, const char *mode)
{
    (void)mode;
    if (fd < 0) {
        errno = EBADF;
        return 0;
    }
    FILE *f = malloc(sizeof(*f));
    if (!f) {
        errno = ENOMEM;
        return 0;
    }
    f->fd = fd;
    f->standard = 0;
    return f;
}

int fclose(FILE *stream)
{
    if (!stream) {
        return EOF;
    }
    if (stream->standard) {
        return 0;
    }
    int rc = close(stream->fd);
    free(stream);
    return rc;
}

size_t fread(void *ptr, size_t size, size_t nmemb, FILE *stream)
{
    if (!ptr || !stream || size == 0) {
        return 0;
    }
    ssize_t n = read(stream->fd, ptr, size * nmemb);
    return n <= 0 ? 0 : (size_t)n / size;
}

size_t fwrite(const void *ptr, size_t size, size_t nmemb, FILE *stream)
{
    if (!ptr || !stream || size == 0) {
        return 0;
    }
    ssize_t n = write(stream->fd, ptr, size * nmemb);
    return n <= 0 ? 0 : (size_t)n / size;
}

int fseek(FILE *stream, long offset, int whence)
{
    return !stream || lseek(stream->fd, offset, whence) < 0 ? -1 : 0;
}

long ftell(FILE *stream)
{
    return stream ? (long)lseek(stream->fd, 0, SEEK_CUR) : -1;
}

int fflush(FILE *stream)
{
    (void)stream;
    return 0;
}

int ferror(FILE *stream)
{
    (void)stream;
    return 0;
}

void clearerr(FILE *stream)
{
    (void)stream;
}

int fileno(FILE *stream)
{
    return stream ? stream->fd : -1;
}

int remove(const char *path)
{
    return unlink(path);
}

int rename(const char *oldpath, const char *newpath)
{
    if (!oldpath || !newpath) {
        errno = EINVAL;
        return -1;
    }

    int in = open(oldpath, O_RDONLY);
    if (in < 0) {
        return -1;
    }

    int out = open(newpath, O_CREAT | O_TRUNC | O_WRONLY, 0644);
    if (out < 0) {
        close(in);
        return -1;
    }

    char buffer[512];
    for (;;) {
        ssize_t n = read(in, buffer, sizeof(buffer));
        if (n < 0) {
            close(in);
            close(out);
            unlink(newpath);
            return -1;
        }
        if (n == 0) {
            break;
        }
        ssize_t off = 0;
        while (off < n) {
            ssize_t written = write(out, buffer + off, (size_t)(n - off));
            if (written <= 0) {
                close(in);
                close(out);
                unlink(newpath);
                return -1;
            }
            off += written;
        }
    }

    close(in);
    close(out);
    return unlink(oldpath);
}
