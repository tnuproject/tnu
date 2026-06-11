#ifndef TNU_LOG_H
#define TNU_LOG_H

void log_init(void);
void log_info(const char *subsystem, const char *fmt, ...);
void log_warn(const char *subsystem, const char *fmt, ...);
void log_error(const char *subsystem, const char *fmt, ...);
void log_debug(const char *subsystem, const char *fmt, ...);
const char *log_buffer(void);

#endif
