/*
 * tnu_curses.c — ncurses implementation for GNU nano on TNU
 * ANSI/VT100 terminal emulation over TNU's raw /dev/tty interface.
 */

#include "tnu_curses.h"
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdio.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <fcntl.h>
#include <poll.h>

#ifndef O_NONBLOCK
#define O_NONBLOCK 04000
#endif

/* ------------------------------------------------------------------ */
/* Global state                                                         */
/* ------------------------------------------------------------------ */

static WINDOW _stdscr = { 24, 80, 0, 0, 0, 0, A_NORMAL };

WINDOW *stdscr = &_stdscr;
WINDOW *curscr = &_stdscr;

int COLS  = 80;
int LINES = 24;
/* refresh_needed is owned by nano's global.c — declared extern in tnu_curses.h */
bool resized = false;

static struct termios orig_tio;
static bool raw_active = false;
static bool colors_started = false;

/* key pushback buffer */
#define UNGETCH_BUF 16
static int ungetch_buf[UNGETCH_BUF];
static int ungetch_len = 0;

/* color pair table: [fg, bg] */
static short cpairs[COLOR_PAIRS][2];

/* ------------------------------------------------------------------ */
/* ANSI helpers                                                         */
/* ------------------------------------------------------------------ */

static void tnu_write(const char *s, size_t n)
{
    write(STDOUT_FILENO, s, n);
}

static void tnu_puts(const char *s)
{
    tnu_write(s, strlen(s));
}

static void tnu_gotoxy(int y, int x)
{
    char buf[32];
    int n = snprintf(buf, sizeof(buf), "\x1b[%d;%dH", y + 1, x + 1);
    tnu_write(buf, (size_t)n);
}

static void tnu_set_attr(attr_t a)
{
    /* reset */
    tnu_puts("\x1b[0m");
    if (a & A_BOLD)      tnu_puts("\x1b[1m");
    if (a & A_UNDERLINE) tnu_puts("\x1b[4m");
    if (a & A_BLINK)     tnu_puts("\x1b[5m");
    if (a & A_REVERSE)   tnu_puts("\x1b[7m");
    if (a & A_DIM)       tnu_puts("\x1b[2m");

    if (colors_started) {
        short pair = (short)PAIR_NUMBER(a);
        if (pair > 0 && pair < COLOR_PAIRS) {
            short fg = cpairs[pair][0];
            short bg = cpairs[pair][1];
            if (fg >= 0 && fg < 8) {
                char buf[16];
                snprintf(buf, sizeof(buf), "\x1b[%dm", 30 + fg);
                tnu_puts(buf);
            }
            if (bg >= 0 && bg < 8) {
                char buf[16];
                snprintf(buf, sizeof(buf), "\x1b[%dm", 40 + bg);
                tnu_puts(buf);
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* Terminal init/teardown                                               */
/* ------------------------------------------------------------------ */

static void get_term_size(void)
{
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 0) {
        LINES = ws.ws_row;
        COLS  = ws.ws_col;
    }
}

WINDOW *initscr(void)
{
    get_term_size();

    _stdscr.rows  = LINES; _stdscr.cols  = COLS;
    /* topwin/midwin/footwin are owned by nano's global.c */
    if (topwin)  { topwin->rows = 1;          topwin->cols = COLS;  topwin->begy = 0; }
    if (midwin)  { midwin->rows = LINES - 2;  midwin->cols = COLS;  midwin->begy = 1; }
    if (footwin) { footwin->rows = 1;         footwin->cols = COLS; footwin->begy = LINES - 1; }

    /* raw mode */
    tcgetattr(STDIN_FILENO, &orig_tio);
    struct termios t = orig_tio;
    t.c_lflag &= (unsigned)~(ECHO | ICANON | ISIG | IEXTEN);
    t.c_iflag &= (unsigned)~(IXON | ICRNL);
    t.c_oflag &= (unsigned)~OPOST;
    t.c_cc[VMIN]  = 1;
    t.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSANOW, &t);
    raw_active = true;

    /* hide cursor, clear screen */
    tnu_puts("\x1b[?25l\x1b[2J\x1b[H");
    return stdscr;
}

int endwin(void)
{
    tnu_puts("\x1b[0m\x1b[2J\x1b[H\x1b[?25h");
    if (raw_active) {
        tcsetattr(STDIN_FILENO, TCSANOW, &orig_tio);
        raw_active = false;
    }
    return OK;
}

bool isendwin(void) { return !raw_active; }
int cbreak(void)  { return OK; }
int nocbreak(void){ return OK; }
int raw(void)     { return OK; }
int noraw(void)   { return OK; }
int echo(void)    { return OK; }
int noecho(void)  { return OK; }
int nl(void)      { return OK; }
int nonl(void)    { return OK; }
int intrflush(WINDOW *w, bool b) { (void)w; (void)b; return OK; }
int meta(WINDOW *w, bool b) { (void)w; (void)b; return OK; }
int napms(int ms)
{
    if (ms <= 0) return OK;
    /* busy-wait — TNU has no usleep from kernel's perspective in user mode
       but posix_stubs.c provides usleep via busy-wait already */
    extern int usleep(unsigned long);
    usleep((unsigned long)ms * 1000);
    return OK;
}

/* ------------------------------------------------------------------ */
/* Window management                                                    */
/* ------------------------------------------------------------------ */

WINDOW *newwin(int nlines, int ncols, int begy, int begx)
{
    WINDOW *w = calloc(1, sizeof(WINDOW));
    if (!w) return NULL;
    w->rows = nlines > 0 ? nlines : LINES;
    w->cols = ncols  > 0 ? ncols  : COLS;
    w->begy = begy;
    w->begx = begx;
    return w;
}

int delwin(WINDOW *win)
{
    if (win && win != stdscr && win != topwin && win != midwin && win != footwin)
        free(win);
    return OK;
}

int mvwin(WINDOW *win, int y, int x)
{
    if (!win) return ERR;
    win->begy = y; win->begx = x;
    return OK;
}

WINDOW *subpad(WINDOW *orig, int nlines, int ncols, int begy, int begx)
{
    (void)orig;
    return newwin(nlines, ncols, begy, begx);
}

int wresize(WINDOW *win, int nlines, int ncols)
{
    if (!win) return ERR;
    win->rows = nlines;
    win->cols = ncols;
    return OK;
}

int resizeterm(int nlines, int ncols)
{
    LINES = nlines; COLS = ncols;
    wresize(stdscr,  nlines, ncols);
    wresize(topwin,  1, ncols);
    wresize(midwin,  nlines - 2, ncols);
    wresize(footwin, 1, ncols);
    footwin->begy = nlines - 1;
    return OK;
}

/* ------------------------------------------------------------------ */
/* Cursor                                                               */
/* ------------------------------------------------------------------ */

int wmove(WINDOW *win, int y, int x)
{
    if (!win) return ERR;
    win->y = y; win->x = x;
    return OK;
}

/* ------------------------------------------------------------------ */
/* Output                                                               */
/* ------------------------------------------------------------------ */

static void win_emit_char(WINDOW *win, char c)
{
    tnu_set_attr(win->attrs);
    tnu_gotoxy(win->begy + win->y, win->begx + win->x);
    char buf[2] = { c, 0 };
    tnu_write(buf, 1);
    win->x++;
    if (win->x >= win->cols) {
        win->x = 0;
        win->y++;
    }
}

int waddch(WINDOW *win, chtype ch)
{
    if (!win) return ERR;
    char c = (char)(ch & A_CHARTEXT);
    if (c < 32 || c == 127) {
        /* control chars — skip or handle tab */
        if (c == '\t') {
            int spaces = 8 - (win->x % 8);
            for (int i = 0; i < spaces; i++) win_emit_char(win, ' ');
        }
        return OK;
    }
    win_emit_char(win, c);
    return OK;
}

int waddstr(WINDOW *win, const char *str)
{
    if (!win || !str) return ERR;
    while (*str) waddch(win, (unsigned char)*str++);
    return OK;
}

int waddnstr(WINDOW *win, const char *str, int n)
{
    if (!win || !str) return ERR;
    for (int i = 0; i < n && str[i]; i++)
        waddch(win, (unsigned char)str[i]);
    return OK;
}

int mvwaddch(WINDOW *win, int y, int x, chtype ch)
{
    wmove(win, y, x); return waddch(win, ch);
}
int mvwaddstr(WINDOW *win, int y, int x, const char *str)
{
    wmove(win, y, x); return waddstr(win, str);
}
int mvwaddnstr(WINDOW *win, int y, int x, const char *str, int n)
{
    wmove(win, y, x); return waddnstr(win, str, n);
}

int wprintw(WINDOW *win, const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    return waddstr(win, buf);
}

int mvwprintw(WINDOW *win, int y, int x, const char *fmt, ...)
{
    wmove(win, y, x);
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    return waddstr(win, buf);
}

int wclrtoeol(WINDOW *win)
{
    if (!win) return ERR;
    tnu_gotoxy(win->begy + win->y, win->begx + win->x);
    tnu_puts("\x1b[K");
    return OK;
}

int wclrtobot(WINDOW *win)
{
    if (!win) return ERR;
    for (int r = win->y; r < win->rows; r++) {
        tnu_gotoxy(win->begy + r, win->begx);
        tnu_puts("\x1b[K");
    }
    return OK;
}

int werase(WINDOW *win)
{
    if (!win) return ERR;
    for (int r = 0; r < win->rows; r++) {
        tnu_gotoxy(win->begy + r, win->begx);
        tnu_puts("\x1b[K");
    }
    win->y = 0; win->x = 0;
    return OK;
}

int wclear(WINDOW *win) { return werase(win); }

int wscrl(WINDOW *win, int n)
{
    (void)win; (void)n;
    return OK;
}

int wredrawln(WINDOW *win, int beg, int num)
{
    (void)win; (void)beg; (void)num;
    return OK;
}

int winsertln(WINDOW *win) { (void)win; return OK; }
int wdeleteln(WINDOW *win) { (void)win; return OK; }

/* ------------------------------------------------------------------ */
/* Refresh                                                              */
/* ------------------------------------------------------------------ */

int wrefresh(WINDOW *win)
{
    if (!win) return ERR;
    /* position cursor at window's current cursor position */
    tnu_gotoxy(win->begy + win->y, win->begx + win->x);
    tnu_puts("\x1b[?25h");
    return OK;
}

int wnoutrefresh(WINDOW *win) { (void)win; return OK; }
int doupdate(void) { return wrefresh(stdscr); }
int refresh(void)  { return wrefresh(stdscr); }

/* ------------------------------------------------------------------ */
/* Attributes                                                           */
/* ------------------------------------------------------------------ */

int wattron(WINDOW *win, attr_t a)  { if (win) win->attrs |= a;  tnu_set_attr(win ? win->attrs : A_NORMAL); return OK; }
int wattroff(WINDOW *win, attr_t a) { if (win) win->attrs &= ~a; tnu_set_attr(win ? win->attrs : A_NORMAL); return OK; }
int wattrset(WINDOW *win, attr_t a) { if (win) win->attrs = a;   tnu_set_attr(win ? win->attrs : A_NORMAL); return OK; }

int wattr_set(WINDOW *win, attr_t a, short pair, void *opts)
{
    (void)opts;
    if (win) win->attrs = a | COLOR_PAIR(pair);
    tnu_set_attr(win ? win->attrs : A_NORMAL);
    return OK;
}

int wattr_get(WINDOW *win, attr_t *a, short *pair, void *opts)
{
    (void)opts;
    if (win && a)    *a    = win->attrs;
    if (win && pair) *pair = (short)PAIR_NUMBER(win->attrs);
    return OK;
}

int wbkgd(WINDOW *win, chtype ch) { (void)win; (void)ch; return OK; }
int scrollok(WINDOW *win, bool b)  { (void)win; (void)b; return OK; }
int idlok(WINDOW *win, bool b)     { (void)win; (void)b; return OK; }

/* ------------------------------------------------------------------ */
/* Colors                                                               */
/* ------------------------------------------------------------------ */

int start_color(void)
{
    colors_started = true;
    for (int i = 0; i < COLOR_PAIRS; i++) {
        cpairs[i][0] = COLOR_WHITE;
        cpairs[i][1] = COLOR_BLACK;
    }
    return OK;
}

int init_pair(short pair, short fg, short bg)
{
    if (pair < 0 || pair >= COLOR_PAIRS) return ERR;
    cpairs[pair][0] = fg;
    cpairs[pair][1] = bg;
    return OK;
}

bool has_colors(void) { return true; }
int use_default_colors(void) { return OK; }

/* ------------------------------------------------------------------ */
/* Input                                                                */
/* ------------------------------------------------------------------ */

static int kbd_fd = -1;

static int open_kbd(void)
{
    if (kbd_fd >= 0) return kbd_fd;
    kbd_fd = open("/dev/input/kbd", O_RDONLY | O_NONBLOCK);
    return kbd_fd;
}

static int try_read_kbd(void)
{
    int fd = open_kbd();
    if (fd < 0) return ERR;

    uint16_t event;
    ssize_t r = read(fd, &event, sizeof(event));
    if (r != sizeof(event)) return ERR;

    /* Ignore release events (bit 15 set) */
    if (event & 0x8000) return ERR;

    return (int)(event & 0x7fff);
}

/* Map TNU special keys to ncurses KEY_* */
static int map_tnu_key(int k)
{
    if (k == 0x101) return KEY_UP;
    if (k == 0x102) return KEY_DOWN;
    if (k == 0x103) return KEY_LEFT;
    if (k == 0x104) return KEY_RIGHT;
    if (k == 0x105) return KEY_HOME;
    if (k == 0x106) return KEY_END;
    if (k == 0x107) return KEY_DC;
    if (k == 0x108) return 3;  /* CTRL+C */
    return k;
}

static int read_byte(void)
{
    unsigned char c;
    ssize_t r = read(STDIN_FILENO, &c, 1);
    if (r != 1) return ERR;
    return (int)c;
}

int wgetch(WINDOW *win)
{
    (void)win;

    if (ungetch_len > 0)
        return ungetch_buf[--ungetch_len];

    /* Use poll to wait for input */
    struct pollfd pfd = { .fd = STDIN_FILENO, .events = POLLIN };
    int ret = poll(&pfd, 1, -1); /* block indefinitely */
    if (ret <= 0) return ERR;

    /* Simple blocking read from stdin */
    int c = read_byte();
    if (c == ERR) return ERR;

    /* Handle escape sequences for special keys */
    if (c == 27) { /* ESC */
        /* Try to read sequence - use non-blocking read for next char */
        unsigned char seq[8];
        ssize_t n = read(STDIN_FILENO, seq, sizeof(seq));
        if (n > 0 && seq[0] == '[' && n >= 2) {
            switch (seq[1]) {
            case 'A': return KEY_UP;
            case 'B': return KEY_DOWN;
            case 'C': return KEY_RIGHT;
            case 'D': return KEY_LEFT;
            case 'H': return KEY_HOME;
            case 'F': return KEY_END;
            }
        }
        return 27;
    }

    if (c == '\r') c = '\n';
    return c;
}

int ungetch(int ch)
{
    if (ungetch_len >= UNGETCH_BUF) return ERR;
    ungetch_buf[ungetch_len++] = ch;
    return OK;
}

int keypad(WINDOW *win, bool bf) { (void)win; (void)bf; return OK; }
int nodelay(WINDOW *win, bool bf) { (void)win; (void)bf; return OK; }
int notimeout(WINDOW *win, bool bf) { (void)win; (void)bf; return OK; }

void beep(void)  { tnu_puts("\a"); }
void flash(void) { tnu_puts("\x1b[?5h"); tnu_puts("\x1b[?5l"); }

int curs_set(int v)
{
    if (v == 0) tnu_puts("\x1b[?25l");
    else        tnu_puts("\x1b[?25h");
    return OK;
}

/* getpwnam/getpwuid are in posix_extra.c — no stubs needed here */

/* ------------------------------------------------------------------ */
/* SIGWINCH / resize handling                                           */
/* ------------------------------------------------------------------ */

void tnu_curses_check_resize(void)
{
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 &&
        (ws.ws_row != LINES || ws.ws_col != COLS)) {
        resizeterm(ws.ws_row, ws.ws_col);
        LINES = ws.ws_row;
        COLS  = ws.ws_col;
        resized = true;
    }
}
