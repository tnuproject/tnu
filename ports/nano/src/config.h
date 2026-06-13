/* TNU build config for GNU nano (madnight/nano) */
#ifndef CONFIG_H
#define CONFIG_H

#define PACKAGE_NAME    "nano"
#define PACKAGE_VERSION "TNU"
#define REVISION        ""

/* regex flag — configured by autoconf upstream, we use REG_EXTENDED */
#define NANO_REG_EXTENDED REG_EXTENDED

/* Features we enable */
#define ENABLE_WRAPPING  1
#define ENABLE_JUSTIFY   1
#define ENABLE_SEARCH    1
#define ENABLE_HISTORIES 1
#define ENABLE_HELP      1
#define ENABLE_COLOR     1
#define ENABLE_MULTIBUFFER 1
#define ENABLE_TABCOMP   1
#define ENABLE_COMMENT   1

/* parse_one_include stub — color.c calls it when ENABLE_NANORC is off */
struct syntaxtype;
static inline void parse_one_include(char *f, struct syntaxtype *s) { (void)f; (void)s; }

/* rcfile stubs — needed when ENABLE_NANORC is off */
static inline void do_rcfiles(void) {}
#define ignore_rcfiles 1

/* Features we disable (no POSIX extras on TNU) */
#undef ENABLE_NLS
#undef ENABLE_NANORC
#undef ENABLE_MOUSE
#undef ENABLE_OPERATINGDIR
#undef ENABLE_SPELLER
#undef ENABLE_LINTER
#undef ENABLE_FORMATTER
#undef ENABLE_BROWSER
#undef ENABLE_LINENUMBERS
#undef ENABLE_WORDCOMPLETION
#undef ENABLE_UTF8
#undef NANO_TINY

/* POSIX feature availability */
#define HAVE_TERMIOS_H  1
#define HAVE_LIMITS_H   1
#define HAVE_SYS_PARAM_H 0
#undef  HAVE_LIBMAGIC
#undef  HAVE_LIBINTL_H
#undef  HAVE_SET_ESCDELAY
#undef  HAVE_USE_DEFAULT_COLORS
#undef  NEED_XOPEN_SOURCE_EXTENDED

/* i18n: disable gettext, use identity macro */
#define _(s)     (s)
#define P_(s,p,n) ((n)==1?(s):(p))
#define N_(s)    (s)
#define gettext_noop(s) (s)

/* ncurses: provided by our tnu_curses shim */
#define HAVE_NCURSES_H 1

#endif /* CONFIG_H */
