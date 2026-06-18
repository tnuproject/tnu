/*
 * tnu_curses.h — ncurses shim for GNU nano on TNU
 *
 * Implements the curses API that nano uses via ANSI/VT100 escape codes.
 * Only the subset actually called by nano is implemented.
 */
#ifndef TNU_CURSES_H
#define TNU_CURSES_H

#include <stdio.h>
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <signal.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <pwd.h>
#include <getopt.h>
#include <glob.h>
#include <locale.h>
#include <langinfo.h>
#include <regex.h>
#include <libgen.h>
#include <wchar.h>
#include <wctype.h>

/* ------------------------------------------------------------------ */
/* Types                                                                */
/* ------------------------------------------------------------------ */

typedef unsigned long chtype;
typedef unsigned long attr_t;

typedef struct _win {
    int rows, cols;
    int y, x;       /* cursor position in window */
    int begy, begx;  /* window origin on screen */
    attr_t attrs;
} WINDOW;

typedef unsigned int mmask_t;
typedef struct { int x, y; mmask_t bstate; } MEVENT;

/* ------------------------------------------------------------------ */
/* Global state (exported as nano expects)                              */
/* ------------------------------------------------------------------ */

extern WINDOW *stdscr;
extern WINDOW *topwin;
extern WINDOW *midwin;
extern WINDOW *footwin;
extern int COLS;
extern int LINES;
/* refresh_needed is defined in nano's global.c */
extern bool refresh_needed;
extern bool resized;

/* ------------------------------------------------------------------ */
/* Color pairs                                                          */
/* ------------------------------------------------------------------ */

#define COLOR_BLACK   0
#define COLOR_RED     1
#define COLOR_GREEN   2
#define COLOR_YELLOW  3
#define COLOR_BLUE    4
#define COLOR_MAGENTA 5
#define COLOR_CYAN    6
#define COLOR_WHITE   7

#define COLOR_PAIRS   256
#define COLORS        8

#define COLOR_PAIR(n)     ((attr_t)(n) << 8)
#define PAIR_NUMBER(a)    (((a) >> 8) & 0xff)

/* ------------------------------------------------------------------ */
/* Attributes                                                           */
/* ------------------------------------------------------------------ */

#define A_NORMAL    0UL
#define A_BOLD      (1UL << 24)
#define A_REVERSE   (1UL << 25)
#define A_UNDERLINE (1UL << 26)
#define A_BLINK     (1UL << 27)
#define A_DIM       (1UL << 28)
#define A_STANDOUT  A_REVERSE
#define A_ITALIC    0UL
#define A_PROTECT   0UL
#define A_INVIS     0UL
#define A_ALTCHARSET 0UL
#define A_ATTRIBUTES (~0xFF000000UL & ~0xFFUL)
#define A_CHARTEXT   0xFFUL

/* ------------------------------------------------------------------ */
/* Keys — must match what wgetch returns                                */
/* ------------------------------------------------------------------ */

#define KEY_MIN       0x101
#define KEY_BREAK     0x101
#define KEY_DOWN      0x102
#define KEY_UP        0x103
#define KEY_LEFT      0x104
#define KEY_RIGHT     0x105
#define KEY_HOME      0x106
#define KEY_BACKSPACE 0x107
#define KEY_F0        0x108
#define KEY_F(n)      (KEY_F0 + (n))
#define KEY_DC        (KEY_F0 + 64)
#define KEY_IC        (KEY_F0 + 65)
#define KEY_NPAGE     (KEY_F0 + 66)
#define KEY_PPAGE     (KEY_F0 + 67)
#define KEY_END       (KEY_F0 + 68)
#define KEY_SDC       (KEY_F0 + 69)
#define KEY_SHOME     (KEY_F0 + 70)
#define KEY_SEND      (KEY_F0 + 71)
#define KEY_RESIZE    (KEY_F0 + 72)
#define KEY_MOUSE     (KEY_F0 + 73)
#define KEY_MAX       (KEY_F0 + 128)

#define ERR   (-1)
#define OK    0
#define TRUE  1
#define FALSE 0

/* ------------------------------------------------------------------ */
/* Terminal init/teardown                                               */
/* ------------------------------------------------------------------ */

WINDOW *initscr(void);
int     endwin(void);
bool    isendwin(void);
int     cbreak(void);
int     nocbreak(void);
int     raw(void);
int     noraw(void);
int     echo(void);
int     noecho(void);
int     nl(void);
int     nonl(void);
int     intrflush(WINDOW *win, bool bf);
int     meta(WINDOW *win, bool bf);
int     napms(int ms);
int     halfdelay(int tenths);

/* ------------------------------------------------------------------ */
/* Window creation/deletion                                             */
/* ------------------------------------------------------------------ */

WINDOW *newwin(int nlines, int ncols, int begy, int begx);
int     delwin(WINDOW *win);
int     mvwin(WINDOW *win, int y, int x);
WINDOW *subpad(WINDOW *orig, int nlines, int ncols, int begy, int begx);
int     wresize(WINDOW *win, int nlines, int ncols);
int     resizeterm(int nlines, int ncols);

/* ------------------------------------------------------------------ */
/* Cursor movement                                                      */
/* ------------------------------------------------------------------ */

int wmove(WINDOW *win, int y, int x);
#define move(y,x)    wmove(stdscr, (y), (x))
#define getyx(w,y,x) do { (y) = (w)->y; (x) = (w)->x; } while (0)
#define getmaxyx(w,y,x) do { (y) = (w)->rows; (x) = (w)->cols; } while (0)
#define getbegyx(w,y,x) do { (y) = (w)->begy; (x) = (w)->begx; } while (0)

/* ------------------------------------------------------------------ */
/* Output                                                               */
/* ------------------------------------------------------------------ */

int waddch(WINDOW *win, chtype ch);
int waddstr(WINDOW *win, const char *str);
int waddnstr(WINDOW *win, const char *str, int n);
int mvwaddch(WINDOW *win, int y, int x, chtype ch);
int mvwaddstr(WINDOW *win, int y, int x, const char *str);
int mvwaddnstr(WINDOW *win, int y, int x, const char *str, int n);
int wprintw(WINDOW *win, const char *fmt, ...);
int mvwprintw(WINDOW *win, int y, int x, const char *fmt, ...);
int wclrtoeol(WINDOW *win);
int wclrtobot(WINDOW *win);
int werase(WINDOW *win);
int wclear(WINDOW *win);
int wscrl(WINDOW *win, int n);
int wredrawln(WINDOW *win, int beg, int num);
int winsertln(WINDOW *win);
int wdeleteln(WINDOW *win);

#define addch(c)          waddch(stdscr, (c))
#define addstr(s)         waddstr(stdscr, (s))
#define clrtoeol()        wclrtoeol(stdscr)
#define clrtobot()        wclrtobot(stdscr)
#define erase()           werase(stdscr)
#define clear()           wclear(stdscr)

/* ------------------------------------------------------------------ */
/* Refresh                                                              */
/* ------------------------------------------------------------------ */

int wrefresh(WINDOW *win);
int wnoutrefresh(WINDOW *win);
int doupdate(void);
int refresh(void);

/* ------------------------------------------------------------------ */
/* Attributes & colors                                                  */
/* ------------------------------------------------------------------ */

int wattron(WINDOW *win, attr_t attrs);
int wattroff(WINDOW *win, attr_t attrs);
int wattrset(WINDOW *win, attr_t attrs);
int wattr_set(WINDOW *win, attr_t attrs, short pair, void *opts);
int wattr_get(WINDOW *win, attr_t *attrs, short *pair, void *opts);
int wbkgd(WINDOW *win, chtype ch);
int scrollok(WINDOW *win, bool bf);
int idlok(WINDOW *win, bool bf);

int start_color(void);
int init_pair(short pair, short fg, short bg);
bool has_colors(void);
int use_default_colors(void);

/* ------------------------------------------------------------------ */
/* Input                                                                */
/* ------------------------------------------------------------------ */

int wgetch(WINDOW *win);
int ungetch(int ch);
int keypad(WINDOW *win, bool bf);
int nodelay(WINDOW *win, bool bf);
int notimeout(WINDOW *win, bool bf);
void beep(void);
void flash(void);
int curs_set(int visibility);

/* Mouse stubs (no mouse on TNU) */
#define mousemask(a,b)     ((mmask_t)0)
#define mouseinterval(t)   0
#define wmouse_trafo(w,y,x,b) FALSE
#define getmouse(ev)       ERR
#define BUTTON1_RELEASED   0x001
#define BUTTON1_PRESSED    0x002
#define BUTTON3_PRESSED    0x010
#define ALL_MOUSE_EVENTS   0xffffff
#define REPORT_MOUSE_POSITION 0x8000000

/* term.h — terminal capability stubs */
#define tigetflag(s)   (-1)
#define tigetnum(s)    (-2)
#define tigetstr(s)    ((char *)-1)
#define tputs(s,n,f)   0
#define tparm(s,...)   (s)
#define tgetstr(id,p)  ((char *)NULL)
#define tgetflag(id)   0
#define tgetnum(id)    (-1)
#define tgoto(cap,c,r) ((char *)NULL)
#define setupterm(t,fd,e) 0

/* Extra key codes used by nano */
#define KEY_ENTER    (KEY_F0 + 80)
#define KEY_CANCEL   (KEY_F0 + 81)
#define KEY_SIC      KEY_IC
#define KEY_SLEFT    (KEY_F0 + 82)
#define KEY_SRIGHT   (KEY_F0 + 83)
#define KEY_SCANCEL  (KEY_F0 + 84)
#define KEY_SSUSPEND (KEY_F0 + 85)
#define KEY_SUSPEND  (KEY_F0 + 86)
#define KEY_BTAB     (KEY_F0 + 87)
#define KEY_SBEG     (KEY_F0 + 88)
#define KEY_BEG      (KEY_F0 + 89)
#define KEY_A1       (KEY_F0 + 90)
#define KEY_A3       (KEY_F0 + 91)
#define KEY_B2       (KEY_F0 + 92)
#define KEY_C1       (KEY_F0 + 93)
#define KEY_C3       (KEY_F0 + 94)
#define define_key(s,k)  0
#define typeahead(fd)    0
extern WINDOW *curscr;

/* sys/param.h — MIN/MAX */
#ifndef MIN
#define MIN(a,b) ((a)<(b)?(a):(b))
#endif
#ifndef MAX
#define MAX(a,b) ((a)>(b)?(a):(b))
#endif

/* magic.h stub */
typedef void *magic_t;
#define MAGIC_NONE 0
static inline magic_t magic_open(int f) { (void)f; return 0; }
static inline void magic_close(magic_t m) { (void)m; }
static inline const char *magic_file(magic_t m, const char *p) { (void)m; (void)p; return 0; }

/* revision.h */
#define GIT_REV ""

#endif /* TNU_CURSES_H */
