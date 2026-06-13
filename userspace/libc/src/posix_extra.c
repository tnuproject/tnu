#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <glob.h>
#include <langinfo.h>
#include <libgen.h>
#include <pwd.h>
#include <regex.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <wchar.h>
#include <wctype.h>

char *optarg;
int optind = 1;
int opterr = 1;
int optopt;

static int option_requires_arg(const char *optstring, int c)
{
    const char *p = strchr(optstring, c);
    return p && p[1] == ':';
}

int getopt(int argc, char *const argv[], const char *optstring)
{
    if (optind >= argc || !argv[optind] || argv[optind][0] != '-' || argv[optind][1] == '\0') {
        return -1;
    }
    if (strcmp(argv[optind], "--") == 0) {
        optind++;
        return -1;
    }
    int c = argv[optind][1];
    optopt = c;
    if (!strchr(optstring, c)) {
        optind++;
        return '?';
    }
    if (option_requires_arg(optstring, c)) {
        if (argv[optind][2]) {
            optarg = &argv[optind][2];
        } else if (optind + 1 < argc) {
            optarg = argv[++optind];
        } else {
            optind++;
            return optstring[0] == ':' ? ':' : '?';
        }
    } else {
        optarg = NULL;
    }
    optind++;
    return c;
}

int getopt_long(int argc, char *const argv[], const char *optstring,
                const struct option *longopts, int *longindex)
{
    if (optind < argc && argv[optind] && strncmp(argv[optind], "--", 2) == 0 && argv[optind][2]) {
        const char *name = argv[optind] + 2;
        const char *eq = strchr(name, '=');
        size_t len = eq ? (size_t)(eq - name) : strlen(name);
        for (int i = 0; longopts && longopts[i].name; i++) {
            if (strlen(longopts[i].name) == len && strncmp(longopts[i].name, name, len) == 0) {
                if (longindex) {
                    *longindex = i;
                }
                if (longopts[i].has_arg == required_argument) {
                    if (eq) {
                        optarg = (char *)eq + 1;
                    } else if (optind + 1 < argc) {
                        optarg = argv[++optind];
                    } else {
                        optind++;
                        return '?';
                    }
                } else {
                    optarg = eq ? (char *)eq + 1 : NULL;
                }
                optind++;
                if (longopts[i].flag) {
                    *longopts[i].flag = longopts[i].val;
                    return 0;
                }
                return longopts[i].val;
            }
        }
        optind++;
        return '?';
    }
    return getopt(argc, argv, optstring);
}

char *getenv(const char *name)
{
    if (!name) {
        return NULL;
    }
    if (strcmp(name, "TERM") == 0) return "tnu";
    if (strcmp(name, "HOME") == 0) return "/root";
    if (strcmp(name, "SHELL") == 0) return "/bin/tsh";
    if (strcmp(name, "TMPDIR") == 0) return "/tmp";
    return NULL;
}

int putenv(char *string)
{
    (void)string;
    return 0;
}

int gethostname(char *name, size_t len)
{
    if (!name || len == 0) {
        errno = EINVAL;
        return -1;
    }
    int fd = open("/etc/hostname", O_RDONLY);
    if (fd < 0) {
        strncpy(name, "tiramisu", len - 1);
        name[len - 1] = '\0';
        return 0;
    }
    ssize_t n = read(fd, name, len - 1);
    close(fd);
    if (n < 0) {
        return -1;
    }
    name[n] = '\0';
    char *nl = strchr(name, '\n');
    if (nl) {
        *nl = '\0';
    }
    return 0;
}

uid_t geteuid(void)
{
    return getuid();
}

int kill(pid_t pid, int sig)
{
    (void)pid;
    (void)sig;
    errno = ENOSYS;
    return -1;
}

pid_t fork(void)
{
    errno = ENOSYS;
    return -1;
}

int pipe(int pipefd[2])
{
    (void)pipefd;
    errno = ENOSYS;
    return -1;
}

pid_t waitpid(pid_t pid, int *status, int options)
{
    (void)options;
    int rc = wait(pid);
    if (status) {
        *status = rc < 0 ? 1 : rc;
    }
    return rc < 0 ? -1 : pid;
}

int execl(const char *path, const char *arg, ...)
{
    char *argv[16];
    int argc = 0;
    va_list ap;
    if (arg) {
        argv[argc++] = (char *)arg;
    }
    va_start(ap, arg);
    while (argc < (int)(sizeof(argv) / sizeof(argv[0])) - 1) {
        char *next = va_arg(ap, char *);
        if (!next) {
            break;
        }
        argv[argc++] = next;
    }
    va_end(ap);
    argv[argc] = NULL;
    return execv(path, argv);
}

long fpathconf(int fd, int name)
{
    (void)fd;
    return name == _PC_PIPE_BUF ? 512 : -1;
}

struct passwd *getpwuid(uid_t uid)
{
    static struct passwd pw;
    static char name[32];
    if (uid == 0) {
        strcpy(name, "root");
        pw.pw_dir = "/root";
    } else {
        strcpy(name, "user");
        pw.pw_dir = "/home/user";
    }
    pw.pw_name = name;
    pw.pw_passwd = "x";
    pw.pw_uid = uid;
    pw.pw_gid = uid == 0 ? 0 : 100;
    pw.pw_gecos = name;
    pw.pw_shell = "/bin/tsh";
    return &pw;
}

struct passwd *getpwent(void)
{
    static int done;
    if (done) {
        return NULL;
    }
    done = 1;
    return getpwuid(0);
}

void setpwent(void) {}
void endpwent(void) {}

char *basename(char *path)
{
    if (!path || !*path) {
        return ".";
    }
    char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

char *dirname(char *path)
{
    static char dot[] = ".";
    if (!path || !*path) {
        return dot;
    }
    char *slash = strrchr(path, '/');
    if (!slash) {
        return dot;
    }
    if (slash == path) {
        slash[1] = '\0';
    } else {
        *slash = '\0';
    }
    return path;
}

static const char *literal_find(const char *haystack, const char *needle, int icase)
{
    size_t needle_len = strlen(needle);
    if (needle_len == 0) {
        return haystack;
    }
    for (const char *p = haystack; *p; p++) {
        size_t i = 0;
        while (i < needle_len && p[i]) {
            char a = p[i], b = needle[i];
            if (icase) {
                if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
                if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
            }
            if (a != b) break;
            i++;
        }
        if (i == needle_len) {
            return p;
        }
    }
    return NULL;
}

int regcomp(regex_t *preg, const char *regex, int cflags)
{
    if (!preg || !regex) {
        return REG_BADPAT;
    }
    preg->pattern = strdup(regex);
    preg->re_nsub = 0;
    (void)cflags;
    return preg->pattern ? 0 : REG_BADPAT;
}

int regexec(const regex_t *preg, const char *string, size_t nmatch,
            regmatch_t pmatch[], int eflags)
{
    if (!preg || !preg->pattern || !string) {
        return REG_NOMATCH;
    }
    const char *start = string;
    const char *end = string + strlen(string);
    if ((eflags & REG_STARTEND) && nmatch > 0) {
        start = string + pmatch[0].rm_so;
        end = string + pmatch[0].rm_eo;
    }
    char *slice = malloc((size_t)(end - start) + 1);
    if (!slice) {
        return REG_NOMATCH;
    }
    memcpy(slice, start, (size_t)(end - start));
    slice[end - start] = '\0';
    const char *found = literal_find(slice, preg->pattern, 0);
    if (!found) {
        free(slice);
        return REG_NOMATCH;
    }
    if (nmatch > 0 && pmatch) {
        pmatch[0].rm_so = (regoff_t)(found - slice + (start - string));
        pmatch[0].rm_eo = pmatch[0].rm_so + (regoff_t)strlen(preg->pattern);
    }
    free(slice);
    return 0;
}

size_t regerror(int errcode, const regex_t *preg, char *errbuf, size_t errbuf_size)
{
    (void)preg;
    const char *msg = errcode == REG_NOMATCH ? "no match" : "bad pattern";
    size_t len = strlen(msg) + 1;
    if (errbuf && errbuf_size) {
        strncpy(errbuf, msg, errbuf_size - 1);
        errbuf[errbuf_size - 1] = '\0';
    }
    return len;
}

void regfree(regex_t *preg)
{
    if (preg && preg->pattern) {
        free(preg->pattern);
        preg->pattern = NULL;
    }
}

clock_t clock(void)
{
    return (clock_t)uptime_ms();
}

time_t time(time_t *tloc)
{
    time_t now = (time_t)(uptime_ms() / 1000);
    if (tloc) {
        *tloc = now;
    }
    return now;
}

int wctomb(char *s, wchar_t wc)
{
    if (s) {
        s[0] = (char)wc;
    }
    return 1;
}

int wcwidth(wchar_t wc)
{
    return wc == 0 ? 0 : 1;
}

int iswblank(wint_t wc)
{
    return wc == ' ' || wc == '\t';
}

char *nl_langinfo(nl_item item)
{
    (void)item;
    return "ASCII";
}

char *realpath(const char *path, char *resolved_path)
{
    if (!path) {
        errno = EINVAL;
        return NULL;
    }

    char cwd[512];
    char tmp[512];
    if (path[0] == '/') {
        strncpy(tmp, path, sizeof(tmp) - 1);
        tmp[sizeof(tmp) - 1] = '\0';
    } else {
        if (getcwd(cwd, sizeof(cwd)) < 0) {
            return NULL;
        }
        snprintf(tmp, sizeof(tmp), "%s/%s", cwd, path);
    }

    char *out = resolved_path ? resolved_path : malloc(strlen(tmp) + 1);
    if (!out) {
        errno = ENOMEM;
        return NULL;
    }

    char stack[64][64];
    int depth = 0;
    const char *p = tmp;
    while (*p) {
        while (*p == '/') p++;
        char part[64];
        size_t len = 0;
        while (*p && *p != '/' && len + 1 < sizeof(part)) {
            part[len++] = *p++;
        }
        part[len] = '\0';
        if (len == 0 || strcmp(part, ".") == 0) {
            continue;
        }
        if (strcmp(part, "..") == 0) {
            if (depth > 0) depth--;
            continue;
        }
        if (depth < 64) {
            strncpy(stack[depth], part, sizeof(stack[depth]) - 1);
            stack[depth][sizeof(stack[depth]) - 1] = '\0';
            depth++;
        }
    }

    if (depth == 0) {
        strcpy(out, "/");
        return out;
    }
    out[0] = '\0';
    for (int i = 0; i < depth; i++) {
        strcat(out, "/");
        strcat(out, stack[i]);
    }
    return out;
}

int mkstemps(char *template_name, int suffixlen)
{
    if (!template_name || suffixlen < 0) {
        errno = EINVAL;
        return -1;
    }
    size_t len = strlen(template_name);
    if (len < (size_t)suffixlen + 6) {
        errno = EINVAL;
        return -1;
    }
    char *xs = template_name + len - suffixlen - 6;
    if (strncmp(xs, "XXXXXX", 6) != 0) {
        errno = EINVAL;
        return -1;
    }

    static const char alphabet[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    uint64_t seed = uptime_ms() ^ ((uint64_t)getpid() << 32);
    for (int attempt = 0; attempt < 128; attempt++) {
        uint64_t v = seed + (uint64_t)attempt * 1103515245u;
        for (int i = 0; i < 6; i++) {
            xs[i] = alphabet[v % (sizeof(alphabet) - 1)];
            v = v / (sizeof(alphabet) - 1) + 17;
        }
        int fd = open(template_name, O_CREAT | O_EXCL | O_RDWR, 0600);
        if (fd >= 0) {
            return fd;
        }
    }
    errno = EEXIST;
    return -1;
}

int mkstemp(char *template_name)
{
    return mkstemps(template_name, 0);
}

void qsort(void *base, size_t nmemb, size_t size,
           int (*compar)(const void *, const void *))
{
    /* insertion sort — sufficient for nano's small arrays */
    char *b = base;
    char *tmp = malloc(size);
    if (!tmp) return;
    for (size_t i = 1; i < nmemb; i++) {
        memcpy(tmp, b + i * size, size);
        size_t j = i;
        while (j > 0 && compar(b + (j - 1) * size, tmp) > 0) {
            memcpy(b + j * size, b + (j - 1) * size, size);
            j--;
        }
        memcpy(b + j * size, tmp, size);
    }
    free(tmp);
}

void *bsearch(const void *key, const void *base, size_t nmemb, size_t size,
              int (*compar)(const void *, const void *))
{
    size_t lo = 0, hi = nmemb;
    const char *b = base;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        int c = compar(key, b + mid * size);
        if (c == 0) return (void *)(b + mid * size);
        if (c < 0) hi = mid; else lo = mid + 1;
    }
    return NULL;
}

long long atoll(const char *s)
{
    long long v = 0;
    int neg = 0;
    while (*s == ' ' || *s == '\t') s++;
    if (*s == '-') { neg = 1; s++; } else if (*s == '+') s++;
    while (*s >= '0' && *s <= '9') { v = v * 10 + (*s++ - '0'); }
    return neg ? -v : v;
}

static char env_buf[256][128];
static size_t env_count;

char *getenv(const char *name);  /* forward — defined in posix_stubs.c */

int setenv(const char *name, const char *value, int overwrite)
{
    if (!name) return -1;
    for (size_t i = 0; i < env_count; i++) {
        if (strncmp(env_buf[i], name, strlen(name)) == 0 &&
            env_buf[i][strlen(name)] == '=') {
            if (!overwrite) return 0;
            snprintf(env_buf[i], sizeof(env_buf[i]), "%s=%s", name, value ? value : "");
            return 0;
        }
    }
    if (env_count < 256) {
        snprintf(env_buf[env_count++], sizeof(env_buf[0]), "%s=%s", name, value ? value : "");
    }
    return 0;
}

int unsetenv(const char *name)
{
    if (!name) return -1;
    for (size_t i = 0; i < env_count; i++) {
        if (strncmp(env_buf[i], name, strlen(name)) == 0 &&
            env_buf[i][strlen(name)] == '=') {
            memmove(env_buf[i], env_buf[i + 1], (env_count - i - 1) * sizeof(env_buf[0]));
            env_count--;
            return 0;
        }
    }
    return 0;
}

int futimens(int fd, const struct timespec times[2])
{
    (void)fd;
    (void)times;
    return 0;
}

int glob(const char *pattern, int flags, int (*errfunc)(const char *, int), glob_t *pglob)
{
    (void)pattern;
    (void)flags;
    (void)errfunc;
    if (pglob) {
        memset(pglob, 0, sizeof(*pglob));
    }
    return GLOB_NOMATCH;
}

void globfree(glob_t *pglob)
{
    (void)pglob;
}
