#include <arch/cpu.h>
#include <arch/keyboard.h>
#include <arch/io.h>
#include <arch/pit.h>
#include <arch/serial.h>
#include <arch/vga.h>
#include <tnu/console.h>
#include <tnu/framebuffer.h>
#include <tnu/printf.h>
#include <tnu/string.h>
#include <tnu/version.h>

#define VGA_WIDTH 80
#define VGA_HEIGHT 25
#define FB_CELL_W 12
#define FB_CELL_H 18
#define FB_GLYPH_SCALE 2
#define FB_MAX_COLS 160
#define FB_MAX_ROWS 64

enum console_backend {
    CONSOLE_BACKEND_VGA,
    CONSOLE_BACKEND_FB,
};

static volatile uint16_t *const vga = (volatile uint16_t *)0xb8000;
static enum console_backend backend = CONSOLE_BACKEND_VGA;
static size_t cols = VGA_WIDTH;
static size_t rows = VGA_HEIGHT;
static size_t row;
static size_t col;
static uint8_t color;
static char *capture_buf;
static size_t capture_size;
static size_t capture_len;
static bool capture_active;
static char fb_cells[FB_MAX_ROWS][FB_MAX_COLS];
static uint8_t fb_attrs[FB_MAX_ROWS][FB_MAX_COLS];
static bool fb_cursor_drawn;
static bool fb_cursor_visible = true;
static size_t fb_cursor_row;
static size_t fb_cursor_col;
static uint64_t fb_cursor_last_tick;

static const uint32_t rgb_palette[16] = {
    0x000000, 0x0000aa, 0x00aa00, 0x00aaaa,
    0xaa0000, 0xaa00aa, 0xaa5500, 0xaaaaaa,
    0x555555, 0x5555ff, 0x55ff55, 0x55ffff,
    0xff5555, 0xff55ff, 0xffff55, 0xffffff,
};

static uint16_t entry(char c)
{
    return (uint16_t)c | ((uint16_t)color << 8);
}

static void move_cursor(void)
{
    if (backend != CONSOLE_BACKEND_VGA) {
        return;
    }
    uint16_t pos = (uint16_t)(row * VGA_WIDTH + col);
    outb(0x3d4, 0x0f);
    outb(0x3d5, (uint8_t)(pos & 0xff));
    outb(0x3d4, 0x0e);
    outb(0x3d5, (uint8_t)((pos >> 8) & 0xff));
}

static const uint8_t *glyph_rows(char c)
{
    unsigned char uc = (unsigned char)c;
    static const uint8_t digits[10][7] = {
        {0x0e, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0e},
        {0x04, 0x0c, 0x04, 0x04, 0x04, 0x04, 0x0e},
        {0x0e, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1f},
        {0x1e, 0x01, 0x01, 0x0e, 0x01, 0x01, 0x1e},
        {0x02, 0x06, 0x0a, 0x12, 0x1f, 0x02, 0x02},
        {0x1f, 0x10, 0x10, 0x1e, 0x01, 0x01, 0x1e},
        {0x06, 0x08, 0x10, 0x1e, 0x11, 0x11, 0x0e},
        {0x1f, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08},
        {0x0e, 0x11, 0x11, 0x0e, 0x11, 0x11, 0x0e},
        {0x0e, 0x11, 0x11, 0x0f, 0x01, 0x02, 0x0c},
    };
    static const uint8_t letters[26][7] = {
        {0x0e, 0x11, 0x11, 0x1f, 0x11, 0x11, 0x11},
        {0x1e, 0x11, 0x11, 0x1e, 0x11, 0x11, 0x1e},
        {0x0f, 0x10, 0x10, 0x10, 0x10, 0x10, 0x0f},
        {0x1e, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1e},
        {0x1f, 0x10, 0x10, 0x1e, 0x10, 0x10, 0x1f},
        {0x1f, 0x10, 0x10, 0x1e, 0x10, 0x10, 0x10},
        {0x0f, 0x10, 0x10, 0x13, 0x11, 0x11, 0x0f},
        {0x11, 0x11, 0x11, 0x1f, 0x11, 0x11, 0x11},
        {0x0e, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0e},
        {0x01, 0x01, 0x01, 0x01, 0x11, 0x11, 0x0e},
        {0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11},
        {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1f},
        {0x11, 0x1b, 0x15, 0x15, 0x11, 0x11, 0x11},
        {0x11, 0x19, 0x15, 0x13, 0x11, 0x11, 0x11},
        {0x0e, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0e},
        {0x1e, 0x11, 0x11, 0x1e, 0x10, 0x10, 0x10},
        {0x0e, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0d},
        {0x1e, 0x11, 0x11, 0x1e, 0x14, 0x12, 0x11},
        {0x0f, 0x10, 0x10, 0x0e, 0x01, 0x01, 0x1e},
        {0x1f, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04},
        {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0e},
        {0x11, 0x11, 0x11, 0x11, 0x11, 0x0a, 0x04},
        {0x11, 0x11, 0x11, 0x15, 0x15, 0x15, 0x0a},
        {0x11, 0x11, 0x0a, 0x04, 0x0a, 0x11, 0x11},
        {0x11, 0x11, 0x0a, 0x04, 0x04, 0x04, 0x04},
        {0x1f, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1f},
    };
    static const uint8_t lower[26][7] = {
        {0x00, 0x00, 0x0e, 0x01, 0x0f, 0x11, 0x0f},
        {0x10, 0x10, 0x1e, 0x11, 0x11, 0x11, 0x1e},
        {0x00, 0x00, 0x0f, 0x10, 0x10, 0x10, 0x0f},
        {0x01, 0x01, 0x0f, 0x11, 0x11, 0x11, 0x0f},
        {0x00, 0x00, 0x0e, 0x11, 0x1f, 0x10, 0x0e},
        {0x06, 0x08, 0x08, 0x1c, 0x08, 0x08, 0x08},
        {0x00, 0x00, 0x0f, 0x11, 0x0f, 0x01, 0x0e},
        {0x10, 0x10, 0x1e, 0x11, 0x11, 0x11, 0x11},
        {0x04, 0x00, 0x0c, 0x04, 0x04, 0x04, 0x0e},
        {0x02, 0x00, 0x06, 0x02, 0x02, 0x12, 0x0c},
        {0x10, 0x10, 0x12, 0x14, 0x18, 0x14, 0x12},
        {0x0c, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0e},
        {0x00, 0x00, 0x1a, 0x15, 0x15, 0x15, 0x15},
        {0x00, 0x00, 0x1e, 0x11, 0x11, 0x11, 0x11},
        {0x00, 0x00, 0x0e, 0x11, 0x11, 0x11, 0x0e},
        {0x00, 0x00, 0x1e, 0x11, 0x1e, 0x10, 0x10},
        {0x00, 0x00, 0x0f, 0x11, 0x0f, 0x01, 0x01},
        {0x00, 0x00, 0x16, 0x19, 0x10, 0x10, 0x10},
        {0x00, 0x00, 0x0f, 0x10, 0x0e, 0x01, 0x1e},
        {0x08, 0x08, 0x1c, 0x08, 0x08, 0x08, 0x06},
        {0x00, 0x00, 0x11, 0x11, 0x11, 0x13, 0x0d},
        {0x00, 0x00, 0x11, 0x11, 0x11, 0x0a, 0x04},
        {0x00, 0x00, 0x11, 0x15, 0x15, 0x15, 0x0a},
        {0x00, 0x00, 0x11, 0x0a, 0x04, 0x0a, 0x11},
        {0x00, 0x00, 0x11, 0x11, 0x0f, 0x01, 0x0e},
        {0x00, 0x00, 0x1f, 0x02, 0x04, 0x08, 0x1f},
    };
    static const uint8_t space[7] = {0, 0, 0, 0, 0, 0, 0};
    static const uint8_t bang[7] = {0x04, 0x04, 0x04, 0x04, 0x04, 0x00, 0x04};
    static const uint8_t quote[7] = {0x0a, 0x0a, 0x0a, 0, 0, 0, 0};
    static const uint8_t hash[7] = {0x0a, 0x1f, 0x0a, 0x0a, 0x1f, 0x0a, 0};
    static const uint8_t dollar[7] = {0x04, 0x0f, 0x14, 0x0e, 0x05, 0x1e, 0x04};
    static const uint8_t percent[7] = {0x18, 0x19, 0x02, 0x04, 0x08, 0x13, 0x03};
    static const uint8_t amp[7] = {0x0c, 0x12, 0x14, 0x08, 0x15, 0x12, 0x0d};
    static const uint8_t apos[7] = {0x04, 0x04, 0x08, 0, 0, 0, 0};
    static const uint8_t lparen[7] = {0x02, 0x04, 0x08, 0x08, 0x08, 0x04, 0x02};
    static const uint8_t rparen[7] = {0x08, 0x04, 0x02, 0x02, 0x02, 0x04, 0x08};
    static const uint8_t star[7] = {0, 0x15, 0x0e, 0x1f, 0x0e, 0x15, 0};
    static const uint8_t plus[7] = {0, 0x04, 0x04, 0x1f, 0x04, 0x04, 0};
    static const uint8_t comma[7] = {0, 0, 0, 0, 0x04, 0x04, 0x08};
    static const uint8_t minus[7] = {0, 0, 0, 0x1f, 0, 0, 0};
    static const uint8_t dot[7] = {0, 0, 0, 0, 0, 0x0c, 0x0c};
    static const uint8_t slash[7] = {0x01, 0x01, 0x02, 0x04, 0x08, 0x10, 0x10};
    static const uint8_t colon[7] = {0, 0x0c, 0x0c, 0, 0x0c, 0x0c, 0};
    static const uint8_t semicolon[7] = {0, 0x0c, 0x0c, 0, 0x04, 0x04, 0x08};
    static const uint8_t less[7] = {0x02, 0x04, 0x08, 0x10, 0x08, 0x04, 0x02};
    static const uint8_t equal[7] = {0, 0, 0x1f, 0, 0x1f, 0, 0};
    static const uint8_t greater[7] = {0x08, 0x04, 0x02, 0x01, 0x02, 0x04, 0x08};
    static const uint8_t question[7] = {0x0e, 0x11, 0x01, 0x02, 0x04, 0, 0x04};
    static const uint8_t at[7] = {0x0e, 0x11, 0x17, 0x15, 0x17, 0x10, 0x0f};
    static const uint8_t lbrack[7] = {0x0e, 0x08, 0x08, 0x08, 0x08, 0x08, 0x0e};
    static const uint8_t backslash[7] = {0x10, 0x10, 0x08, 0x04, 0x02, 0x01, 0x01};
    static const uint8_t rbrack[7] = {0x0e, 0x02, 0x02, 0x02, 0x02, 0x02, 0x0e};
    static const uint8_t caret[7] = {0x04, 0x0a, 0x11, 0, 0, 0, 0};
    static const uint8_t under[7] = {0, 0, 0, 0, 0, 0, 0x1f};
    static const uint8_t grave[7] = {0x08, 0x04, 0x02, 0, 0, 0, 0};
    static const uint8_t lbrace[7] = {0x02, 0x04, 0x04, 0x08, 0x04, 0x04, 0x02};
    static const uint8_t pipe[7] = {0x04, 0x04, 0x04, 0, 0x04, 0x04, 0x04};
    static const uint8_t rbrace[7] = {0x08, 0x04, 0x04, 0x02, 0x04, 0x04, 0x08};
    static const uint8_t tilde[7] = {0, 0, 0x08, 0x15, 0x02, 0, 0};

    if (c >= '0' && c <= '9') {
        return digits[c - '0'];
    }
    if (c >= 'a' && c <= 'z') {
        return lower[c - 'a'];
    }
    if (c >= 'A' && c <= 'Z') {
        return letters[c - 'A'];
    }

    switch (uc) {
    case 0x82: return lower['e' - 'a'];
    case 0x85: return lower['a' - 'a'];
    case 0x8a: return lower['e' - 'a'];
    case 0x8d: return lower['i' - 'a'];
    case 0x95: return lower['o' - 'a'];
    case 0x97: return lower['u' - 'a'];
    default:
        break;
    }

    switch (c) {
    case ' ': return space;
    case '!': return bang;
    case '"': return quote;
    case '#': return hash;
    case '$': return dollar;
    case '%': return percent;
    case '&': return amp;
    case '\'': return apos;
    case '(': return lparen;
    case ')': return rparen;
    case '*': return star;
    case '+': return plus;
    case ',': return comma;
    case '-': return minus;
    case '.': return dot;
    case '/': return slash;
    case ':': return colon;
    case ';': return semicolon;
    case '<': return less;
    case '=': return equal;
    case '>': return greater;
    case '?': return question;
    case '@': return at;
    case '[': return lbrack;
    case '\\': return backslash;
    case ']': return rbrack;
    case '^': return caret;
    case '_': return under;
    case '`': return grave;
    case '{': return lbrace;
    case '|': return pipe;
    case '}': return rbrace;
    case '~': return tilde;
    default: return question;
    }
}

static uint32_t attr_fg_rgb(uint8_t attr)
{
    return rgb_palette[attr & 0x0f];
}

static uint32_t attr_bg_rgb(uint8_t attr)
{
    return rgb_palette[(attr >> 4) & 0x0f];
}

static uint32_t bg_rgb(void)
{
    return attr_bg_rgb(color);
}

static void fb_draw_char(size_t cx, size_t cy, char c, uint8_t attr, bool inverse)
{
    uint32_t px = (uint32_t)(cx * FB_CELL_W);
    uint32_t py = (uint32_t)(cy * FB_CELL_H);
    const uint8_t *glyph = glyph_rows(c);
    uint32_t fg = attr_fg_rgb(attr);
    uint32_t bg = attr_bg_rgb(attr);

    if (inverse) {
        uint32_t tmp = fg;
        fg = bg;
        bg = tmp;
    }

    framebuffer_fillrect(px, py, FB_CELL_W, FB_CELL_H, bg);
    for (uint32_t gy = 0; gy < 7; gy++) {
        for (uint32_t gx = 0; gx < 5; gx++) {
            if (!(glyph[gy] & (uint8_t)(1u << (4 - gx)))) {
                continue;
            }
            for (uint32_t sy = 0; sy < FB_GLYPH_SCALE; sy++) {
                for (uint32_t sx = 0; sx < FB_GLYPH_SCALE; sx++) {
                    framebuffer_putpixel(px + 1 + gx * FB_GLYPH_SCALE + sx,
                                         py + 2 + gy * FB_GLYPH_SCALE + sy,
                                         fg);
                }
            }
        }
    }
}

static void fb_redraw_cell(size_t cx, size_t cy)
{
    if (backend != CONSOLE_BACKEND_FB || cx >= cols || cy >= rows ||
        cx >= FB_MAX_COLS || cy >= FB_MAX_ROWS) {
        return;
    }
    fb_draw_char(cx, cy, fb_cells[cy][cx], fb_attrs[cy][cx], false);
}

static void fb_put_cell(size_t cx, size_t cy, char c)
{
    if (backend != CONSOLE_BACKEND_FB || cx >= cols || cy >= rows ||
        cx >= FB_MAX_COLS || cy >= FB_MAX_ROWS) {
        return;
    }
    fb_cells[cy][cx] = c;
    fb_attrs[cy][cx] = color;
    fb_redraw_cell(cx, cy);
}

static void fb_hide_cursor(void)
{
    if (!fb_cursor_drawn) {
        return;
    }
    fb_cursor_drawn = false;
    fb_redraw_cell(fb_cursor_col, fb_cursor_row);
}

static void fb_show_cursor(void)
{
    if (backend != CONSOLE_BACKEND_FB || capture_active ||
        row >= rows || col >= cols || row >= FB_MAX_ROWS || col >= FB_MAX_COLS) {
        return;
    }
    if (!fb_cursor_visible) {
        fb_cursor_row = row;
        fb_cursor_col = col;
        return;
    }
    fb_draw_char(col, row, fb_cells[row][col], fb_attrs[row][col], true);
    fb_cursor_row = row;
    fb_cursor_col = col;
    fb_cursor_drawn = true;
}

static void fb_cursor_reset_blink(void)
{
    if (backend != CONSOLE_BACKEND_FB) {
        return;
    }
    fb_hide_cursor();
    fb_cursor_visible = true;
    fb_cursor_last_tick = pit_ticks();
    fb_show_cursor();
}

static void fb_reset_cells(void)
{
    for (size_t y = 0; y < FB_MAX_ROWS; y++) {
        for (size_t x = 0; x < FB_MAX_COLS; x++) {
            fb_cells[y][x] = ' ';
            fb_attrs[y][x] = color;
        }
    }
}

static void fb_scroll(void)
{
    const struct framebuffer_info *fb = framebuffer_info();
    uint8_t *base = (uint8_t *)fb->address;
    size_t scroll_bytes = FB_CELL_H * fb->pitch;
    size_t keep_bytes = (size_t)(fb->height - FB_CELL_H) * fb->pitch;
    memmove(base, base + scroll_bytes, keep_bytes);
    framebuffer_fillrect(0, fb->height - FB_CELL_H, fb->width, FB_CELL_H, bg_rgb());
    if (rows > 1) {
        memmove(fb_cells[0], fb_cells[1], (rows - 1) * sizeof(fb_cells[0]));
        memmove(fb_attrs[0], fb_attrs[1], (rows - 1) * sizeof(fb_attrs[0]));
    }
    for (size_t x = 0; x < cols && x < FB_MAX_COLS; x++) {
        fb_cells[rows - 1][x] = ' ';
        fb_attrs[rows - 1][x] = color;
    }
}

static void scroll(void)
{
    if (row < rows) {
        return;
    }
    if (backend == CONSOLE_BACKEND_FB) {
        fb_scroll();
    } else {
        for (size_t y = 1; y < VGA_HEIGHT; y++) {
            for (size_t x = 0; x < VGA_WIDTH; x++) {
                vga[(y - 1) * VGA_WIDTH + x] = vga[y * VGA_WIDTH + x];
            }
        }
        for (size_t x = 0; x < VGA_WIDTH; x++) {
            vga[(VGA_HEIGHT - 1) * VGA_WIDTH + x] = entry(' ');
        }
    }
    row = rows - 1;
}

void console_set_color(uint8_t fg, uint8_t bg)
{
    color = (uint8_t)(fg | (bg << 4));
}

void console_clear(void)
{
    fb_cursor_drawn = false;
    fb_cursor_visible = true;
    fb_cursor_last_tick = pit_ticks();
    if (backend == CONSOLE_BACKEND_FB) {
        const struct framebuffer_info *fb = framebuffer_info();
        framebuffer_fillrect(0, 0, fb->width, fb->height, bg_rgb());
        fb_reset_cells();
    } else {
        for (size_t y = 0; y < VGA_HEIGHT; y++) {
            for (size_t x = 0; x < VGA_WIDTH; x++) {
                vga[y * VGA_WIDTH + x] = entry(' ');
            }
        }
    }
    row = 0;
    col = 0;
    move_cursor();
    fb_show_cursor();
}

void console_init(void)
{
    if (framebuffer_is_graphics()) {
        const struct framebuffer_info *fb = framebuffer_info();
        backend = CONSOLE_BACKEND_FB;
        cols = fb->width / FB_CELL_W;
        rows = fb->height / FB_CELL_H;
        if (cols < 20) {
            cols = 20;
        }
        if (rows < 8) {
            rows = 8;
        }
        if (cols > FB_MAX_COLS) {
            cols = FB_MAX_COLS;
        }
        if (rows > FB_MAX_ROWS) {
            rows = FB_MAX_ROWS;
        }
    } else {
        backend = CONSOLE_BACKEND_VGA;
        cols = VGA_WIDTH;
        rows = VGA_HEIGHT;
        vga_init_text_mode();
    }
    console_set_color(CONSOLE_LIGHT_GREY, CONSOLE_BLACK);
    console_clear();
}

void console_putc(char c)
{
    if (capture_active) {
        if (capture_buf && capture_len + 1 < capture_size) {
            capture_buf[capture_len++] = c;
            capture_buf[capture_len] = '\0';
        }
        return;
    }
    serial_write_char(c);
    fb_hide_cursor();
    if (c == '\n') {
        col = 0;
        row++;
    } else if (c == '\r') {
        col = 0;
    } else if (c == '\b') {
        if (col > 0) {
            col--;
            if (backend == CONSOLE_BACKEND_FB) {
                fb_put_cell(col, row, ' ');
            } else {
                vga[row * VGA_WIDTH + col] = entry(' ');
            }
        }
    } else if (c == '\t') {
        for (int i = 0; i < 4; i++) {
            console_putc(' ');
        }
        return;
    } else {
        if (backend == CONSOLE_BACKEND_FB) {
            fb_put_cell(col, row, c);
        } else {
            vga[row * VGA_WIDTH + col] = entry(c);
        }
        col++;
        if (col >= cols) {
            col = 0;
            row++;
        }
    }
    scroll();
    move_cursor();
    fb_cursor_reset_blink();
}

void console_write(const char *s)
{
    while (*s) {
        console_putc(*s++);
    }
}

void console_write_n(const char *s, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        console_putc(s[i]);
    }
}

void console_cursor_left(void)
{
    serial_write("\x1b[D");
    fb_hide_cursor();
    if (col > 0) {
        col--;
    } else if (row > 0) {
        row--;
        col = cols - 1;
    }
    move_cursor();
    fb_cursor_reset_blink();
}

void console_cursor_right(void)
{
    serial_write("\x1b[C");
    fb_hide_cursor();
    if (col + 1 < cols) {
        col++;
    } else if (row + 1 < rows) {
        row++;
        col = 0;
    }
    move_cursor();
    fb_cursor_reset_blink();
}

void console_cursor_blink_tick(void)
{
    if (backend != CONSOLE_BACKEND_FB || capture_active) {
        return;
    }
    uint64_t now = pit_ticks();
    if (now - fb_cursor_last_tick < 50) {
        return;
    }
    fb_hide_cursor();
    fb_cursor_visible = !fb_cursor_visible;
    fb_cursor_last_tick = now;
    fb_show_cursor();
}

void console_capture_begin(char *buf, size_t size)
{
    capture_buf = buf;
    capture_size = size;
    capture_len = 0;
    capture_active = true;
    if (capture_buf && capture_size) {
        capture_buf[0] = '\0';
    }
}

size_t console_capture_end(void)
{
    size_t len = capture_len;
    capture_active = false;
    capture_buf = NULL;
    capture_size = 0;
    capture_len = 0;
    return len;
}

int console_getchar(void)
{
    for (;;) {
        int c = serial_read_char();
        if (c >= 0) {
            if (c == '\r') {
                return '\n';
            }
            return c;
        }
        c = keyboard_try_getchar();
        if (c >= 0) {
            return c;
        }
        console_cursor_blink_tick();
        cpu_pause();
    }
}

void console_banner(void)
{
    console_set_color(CONSOLE_LIGHT_CYAN, CONSOLE_BLACK);
    kprintf("TNU %s \"%s\"\n", TNU_VERSION, TNU_CODENAME);
    console_set_color(CONSOLE_LIGHT_GREY, CONSOLE_BLACK);
}
