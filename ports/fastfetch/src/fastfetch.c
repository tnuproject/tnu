/*
 * fastfetch.c — Tiramisu system information tool
 *
 * Displays an ASCII logo alongside key system stats read from /proc and /etc.
 * Designed to run on TNU/Tiramisu without any external dependencies.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <tnu/syscall.h>

/* ------------------------------------------------------------------ */
/* ANSI colour helpers                                                  */
/* ------------------------------------------------------------------ */

#define ANSI_RESET   "\x1b[0m"
#define ANSI_BOLD    "\x1b[1m"
#define ANSI_BLUE    "\x1b[34m"
#define ANSI_CYAN    "\x1b[36m"
#define ANSI_YELLOW  "\x1b[33m"
#define ANSI_WHITE   "\x1b[37m"
#define ANSI_BRIGHT_BLUE  "\x1b[94m"
#define ANSI_BRIGHT_CYAN  "\x1b[96m"

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

/* Read a whole small file into buf (NUL-terminated), return bytes read */
static int read_file_str(const char *path, char *buf, int maxlen)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        buf[0] = '\0';
        return -1;
    }
    int n = (int)read(fd, buf, (size_t)(maxlen - 1));
    close(fd);
    if (n < 0) n = 0;
    buf[n] = '\0';
    return n;
}

/* Extract the value of a "KEY=VALUE" or "KEY: VALUE" line from a buffer */
static int extract_kv(const char *buf, const char *key, char sep,
                      char *out, int outlen)
{
    size_t klen = strlen(key);
    const char *p = buf;
    while (*p) {
        if (strncmp(p, key, klen) == 0 && p[klen] == sep) {
            const char *val = p + klen + 1;
            /* strip leading spaces and tabs */
            while (*val == ' ' || *val == '\t') val++;
            /* strip surrounding quotes (for os-release style) */
            if (*val == '"') {
                val++;
                const char *end = strchr(val, '"');
                int len = end ? (int)(end - val) : (int)strlen(val);
                if (len >= outlen) len = outlen - 1;
                memcpy(out, val, (size_t)len);
                out[len] = '\0';
            } else {
                int len = 0;
                while (val[len] && val[len] != '\n' && len < outlen - 1) len++;
                memcpy(out, val, (size_t)len);
                out[len] = '\0';
            }
            return 1;
        }
        /* advance to next line */
        while (*p && *p != '\n') p++;
        if (*p == '\n') p++;
    }
    out[0] = '\0';
    return 0;
}

/* Parse "MemTotal: NNN kB" style line and return the number */
static unsigned long long extract_kb(const char *buf, const char *key)
{
    char val[64];
    if (!extract_kv(buf, key, ':', val, sizeof(val))) return 0;
    return (unsigned long long)atoi(val);
}

/* Strip trailing newline / spaces */
static void trim_nl(char *s)
{
    int len = (int)strlen(s);
    while (len > 0 && (s[len - 1] == '\n' || s[len - 1] == '\r' ||
                       s[len - 1] == ' '))
        s[--len] = '\0';
}

/* Format bytes into human-readable string */
static void fmt_bytes(unsigned long long kb, char *out, int outlen)
{
    if (kb >= 1024 * 1024) {
        unsigned long long gib = kb / (1024 * 1024);
        unsigned long long frac = ((kb % (1024 * 1024)) * 10) / (1024 * 1024);
        snprintf(out, (size_t)outlen, "%llu.%llu GiB", gib, frac);
    } else if (kb >= 1024) {
        unsigned long long mib = kb / 1024;
        snprintf(out, (size_t)outlen, "%llu MiB", mib);
    } else {
        snprintf(out, (size_t)outlen, "%llu KiB", kb);
    }
}

/* Format uptime seconds into "Xh Ym Zs" */
static void fmt_uptime(unsigned long long secs, char *out, int outlen)
{
    unsigned long long h = secs / 3600;
    unsigned long long m = (secs % 3600) / 60;
    unsigned long long s = secs % 60;
    if (h > 0)
        snprintf(out, (size_t)outlen, "%lluh %llum %llus", h, m, s);
    else if (m > 0)
        snprintf(out, (size_t)outlen, "%llum %llus", m, s);
    else
        snprintf(out, (size_t)outlen, "%llus", s);
}

/* ------------------------------------------------------------------ */
/* Logo                                                                 */
/* ------------------------------------------------------------------ */

/* The ASCII logo is stored in /etc/sysfetch-logo on the rootfs.
 * If not present we fall back to a compact built-in. */
#define LOGO_PATH "/etc/fastfetch-logo"

static const char *builtin_logo[] = {
    "Tiramisù",
    NULL
};

/* ------------------------------------------------------------------ */
/* Info items                                                           */
/* ------------------------------------------------------------------ */

typedef struct {
    const char *label;
    char value[256];
} info_item;

#define MAX_ITEMS 20

static int n_items = 0;
static info_item items[MAX_ITEMS];

static void add_item(const char *label, const char *fmt, ...)
{
    if (n_items >= MAX_ITEMS) return;
    va_list ap;
    va_start(ap, fmt);
    items[n_items].label = label;
    vsnprintf(items[n_items].value, sizeof(items[n_items].value), fmt, ap);
    va_end(ap);
    n_items++;
}

/* ------------------------------------------------------------------ */
/* Gather                                                               */
/* ------------------------------------------------------------------ */

static void gather_info(void)
{
    char buf[2048];
    char val[256];

    /* OS */
    read_file_str("/etc/os-release", buf, (int)sizeof(buf));
    if (extract_kv(buf, "PRETTY_NAME", '=', val, sizeof(val)))
        add_item("OS", "%s", val);
    else
        add_item("OS", "Tiramisu");

    /* Kernel version */
    read_file_str("/proc/version", buf, (int)sizeof(buf));
    trim_nl(buf);
    if (buf[0])
        add_item("Kernel", "%s", buf);

    /* Hostname */
    read_file_str("/etc/hostname", val, (int)sizeof(val));
    trim_nl(val);
    if (val[0])
        add_item("Host", "%s", val);
    else
        add_item("Host", "tiramisu");

    /* Shell */
    add_item("Shell", "tsh");

    /* CPU */
    read_file_str("/proc/cpuinfo", buf, (int)sizeof(buf));
    if (extract_kv(buf, "model name", ':', val, sizeof(val))) {
        trim_nl(val);
        add_item("CPU", "%s", val);
    } else {
        add_item("CPU", "x86_64");
    }

    /* Memory */
    read_file_str("/proc/meminfo", buf, (int)sizeof(buf));
    unsigned long long mem_total  = extract_kb(buf, "MemTotal");
    unsigned long long mem_usable = extract_kb(buf, "MemUsable");
    if (mem_total > 0) {
        char used_s[32], total_s[32];
        unsigned long long mem_used = mem_total > mem_usable ?
                                      mem_total - mem_usable : 0;
        fmt_bytes(mem_used,  used_s,  (int)sizeof(used_s));
        fmt_bytes(mem_total, total_s, (int)sizeof(total_s));
        add_item("Memory", "%s / %s", used_s, total_s);
    }

    /* Uptime */
    read_file_str("/proc/uptime", buf, (int)sizeof(buf));
    unsigned long long up_secs = (unsigned long long)atoi(buf);
    if (up_secs > 0) {
        char up_s[64];
        fmt_uptime(up_secs, up_s, (int)sizeof(up_s));
        add_item("Uptime", "%s", up_s);
    }

    /* Display */
    read_file_str("/proc/framebuffer", buf, (int)sizeof(buf));
    char fb_type[32], fb_w[16], fb_h[16];
    extract_kv(buf, "type",   ':', fb_type, sizeof(fb_type));
    extract_kv(buf, "width",  ':', fb_w,    sizeof(fb_w));
    extract_kv(buf, "height", ':', fb_h,    sizeof(fb_h));
    trim_nl(fb_type); trim_nl(fb_w); trim_nl(fb_h);
    if (fb_w[0] && fb_h[0])
        add_item("Display", "%sx%s (%s)", fb_w, fb_h, fb_type);

    /* Terminal */
    add_item("Terminal", "tnu-vt");
}

/* ------------------------------------------------------------------ */
/* Render                                                               */
/* ------------------------------------------------------------------ */

static void render(const char **logo_lines, int n_logo)
{
    /* Determine terminal width for padding */
    int term_cols = 80;
    {
        struct syscall_winsize ws;
        int ttyfd = open("/dev/tty", O_RDONLY);
        if (ttyfd >= 0) {
            if (ioctl(ttyfd, TNU_IOCTL_TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0)
                term_cols = ws.ws_col;
            close(ttyfd);
        }
    }
    (void)term_cols;

    /* Find the longest logo line for padding */
    int logo_w = 0;
    for (int i = 0; i < n_logo; i++) {
        int len = (int)strlen(logo_lines[i]);
        if (len > logo_w) logo_w = len;
    }

    int rows = n_logo > n_items ? n_logo : n_items;

    printf("\n");
    for (int r = 0; r < rows; r++) {
        /* Logo column */
        if (r < n_logo) {
            printf(ANSI_BRIGHT_BLUE ANSI_BOLD "%s" ANSI_RESET, logo_lines[r]);
            /* Pad to logo_w */
            int pad = logo_w - (int)strlen(logo_lines[r]);
            for (int p = 0; p < pad; p++) putchar(' ');
        } else {
            for (int p = 0; p < logo_w; p++) putchar(' ');
        }

        printf("  ");

        /* Info column */
        if (r < n_items) {
            printf(ANSI_BOLD ANSI_CYAN "%-10s" ANSI_RESET
                   ANSI_WHITE ": %s" ANSI_RESET,
                   items[r].label, items[r].value);
        }

        printf("\n");
    }
    printf("\n");

    /* Colour palette strip */
    printf("  ");
    for (int p = 0; p < logo_w + 2; p++) putchar(' ');
    for (int c = 40; c <= 47; c++)
        printf("\x1b[%dm  " ANSI_RESET, c);
    printf("\n");
    printf("  ");
    for (int p = 0; p < logo_w + 2; p++) putchar(' ');
    for (int c = 100; c <= 107; c++)
        printf("\x1b[%dm  " ANSI_RESET, c);
    printf("\n\n");
}

/* ------------------------------------------------------------------ */
/* main                                                                 */
/* ------------------------------------------------------------------ */

int main(void)
{
    gather_info();

    /* Try to load logo from file */
    static char logo_buf[1024];
    static const char *logo_ptrs[32];
    int n_logo = 0;

    int n = read_file_str(LOGO_PATH, logo_buf, (int)sizeof(logo_buf));
    if (n > 0) {
        /* Split into lines */
        char *p = logo_buf;
        while (*p && n_logo < 31) {
            logo_ptrs[n_logo++] = p;
            char *nl = strchr(p, '\n');
            if (!nl) break;
            *nl = '\0';
            p = nl + 1;
        }
        logo_ptrs[n_logo] = NULL;
    }

    if (n_logo == 0) {
        /* Use built-in logo */
        while (builtin_logo[n_logo]) {
            logo_ptrs[n_logo] = builtin_logo[n_logo];
            n_logo++;
        }
    }

    render(logo_ptrs, n_logo);
    return 0;
}
