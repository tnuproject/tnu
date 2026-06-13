#ifndef TNU_CONSOLE_H
#define TNU_CONSOLE_H

#include <tnu/types.h>

enum console_color {
    CONSOLE_BLACK = 0,
    CONSOLE_BLUE = 1,
    CONSOLE_GREEN = 2,
    CONSOLE_CYAN = 3,
    CONSOLE_RED = 4,
    CONSOLE_MAGENTA = 5,
    CONSOLE_BROWN = 6,
    CONSOLE_LIGHT_GREY = 7,
    CONSOLE_DARK_GREY = 8,
    CONSOLE_LIGHT_BLUE = 9,
    CONSOLE_LIGHT_GREEN = 10,
    CONSOLE_LIGHT_CYAN = 11,
    CONSOLE_LIGHT_RED = 12,
    CONSOLE_LIGHT_MAGENTA = 13,
    CONSOLE_YELLOW = 14,
    CONSOLE_WHITE = 15,
};

void console_init(void);
void console_set_color(uint8_t fg, uint8_t bg);
void console_clear(void);
void console_putc(char c);
void console_write(const char *s);
void console_write_n(const char *s, size_t n);
void console_cursor_left(void);
void console_cursor_right(void);
void console_cursor_blink_tick(void);
size_t console_columns(void);
size_t console_rows(void);
size_t console_pixel_width(void);
size_t console_pixel_height(void);
size_t console_tty_count(void);
size_t console_active_tty(void);
int console_switch_tty(size_t tty_index);
void console_capture_begin(char *buf, size_t size);
size_t console_capture_end(void);
int console_getchar(void);
void console_banner(void);

#endif
