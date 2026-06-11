#include <stdarg.h>
#include <arch/serial.h>
#include <tnu/console.h>
#include <tnu/log.h>
#include <tnu/printf.h>
#include <tnu/string.h>

#define LOG_BUFFER_SIZE 16384

static char log_ring[LOG_BUFFER_SIZE];
static size_t log_len;

static void append_log(const char *line)
{
    size_t n = strlen(line);
    if (n >= LOG_BUFFER_SIZE) {
        line += n - LOG_BUFFER_SIZE + 1;
        n = strlen(line);
    }
    if (log_len + n >= LOG_BUFFER_SIZE) {
        size_t drop = (log_len + n) - LOG_BUFFER_SIZE + 1;
        memmove(log_ring, log_ring + drop, log_len - drop);
        log_len -= drop;
    }
    memcpy(log_ring + log_len, line, n);
    log_len += n;
    log_ring[log_len] = '\0';
}

static void vlog(const char *level, const char *subsystem, bool mirror_console,
                 const char *fmt, va_list ap)
{
    char msg[512];
    char line[640];
    kvsnprintf(msg, sizeof(msg), fmt, ap);
    ksnprintf(line, sizeof(line), "[%s] %s: %s\n", level, subsystem, msg);
    append_log(line);
    serial_write(line);
    if (mirror_console) {
        console_write(line);
    }
}

void log_init(void)
{
    log_len = 0;
    log_ring[0] = '\0';
}

void log_info(const char *subsystem, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vlog("info", subsystem, false, fmt, ap);
    va_end(ap);
}

void log_warn(const char *subsystem, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vlog("warn", subsystem, true, fmt, ap);
    va_end(ap);
}

void log_error(const char *subsystem, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vlog("error", subsystem, true, fmt, ap);
    va_end(ap);
}

void log_debug(const char *subsystem, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vlog("debug", subsystem, false, fmt, ap);
    va_end(ap);
}

const char *log_buffer(void)
{
    return log_ring;
}
