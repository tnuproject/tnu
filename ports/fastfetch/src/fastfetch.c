/*
 * SPDX-License-Identifier: MIT
 *
 * Fastfetch TNU port.
 *
 * Upstream: https://github.com/fastfetch-cli/fastfetch
 * Branch inspected for this port: dev
 *
 * This is not a neofetch-style clone.  It keeps Fastfetch's module-oriented
 * model, CLI vocabulary, output modes, logo handling, and detector split, while
 * replacing OS-specific detector backends with Tiramisu/TNU native backends.
 * The full upstream tree expects a hosted libc, CMake feature probes, pthreads,
 * JSONC config plumbing, and many platform-specific detector files.  TNU does
 * not expose all of that yet, so this port is the native backend slice that can
 * run inside TNU today.
 */

#include <fcntl.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <tnu/syscall.h>

#define FASTFETCH_PROJECT_NAME "Fastfetch"
#define FASTFETCH_TNU_PORT_VERSION "2.0-tnu"
#define FASTFETCH_UPSTREAM_URL "https://github.com/fastfetch-cli/fastfetch"
#define FF_STRBUF_INLINE 256
#define FF_MAX_MODULES 32
#define FF_MAX_LOGO_LINES 32
#define FF_MAX_LOGO_TEXT 2048

#define FF_COLOR_RESET "\x1b[0m"
#define FF_COLOR_BOLD "\x1b[1m"
#define FF_COLOR_TITLE "\x1b[1;36m"
#define FF_COLOR_KEY "\x1b[1;34m"
#define FF_COLOR_LOGO "\x1b[1;94m"
#define FF_COLOR_VALUE "\x1b[37m"

typedef struct FFstrbuf {
    char chars[FF_STRBUF_INLINE];
    size_t length;
} FFstrbuf;

typedef struct FFModuleResult {
    const char *name;
    FFstrbuf value;
    bool ok;
} FFModuleResult;

typedef struct FFOptions {
    bool json;
    bool pipe;
    bool showLogo;
    bool showColors;
    const char *structure;
} FFOptions;

typedef struct FFContext {
    FFOptions options;
    FFModuleResult modules[FF_MAX_MODULES];
    size_t moduleCount;
} FFContext;

static void ffStrbufClear(FFstrbuf *buf)
{
    buf->chars[0] = '\0';
    buf->length = 0;
}

static void ffStrbufSetS(FFstrbuf *buf, const char *text)
{
    if (!text) {
        text = "";
    }
    strncpy(buf->chars, text, sizeof(buf->chars) - 1);
    buf->chars[sizeof(buf->chars) - 1] = '\0';
    buf->length = strlen(buf->chars);
}

static void ffStrbufSetF(FFstrbuf *buf, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf->chars, sizeof(buf->chars), fmt, ap);
    va_end(ap);
    buf->chars[sizeof(buf->chars) - 1] = '\0';
    buf->length = strlen(buf->chars);
}

static int ffReadFileBuffer(const char *path, char *buf, size_t size)
{
    if (!buf || size == 0) {
        return -1;
    }
    int fd = open(path, O_RDONLY, 0);
    if (fd < 0) {
        buf[0] = '\0';
        return -1;
    }
    ssize_t n = read(fd, buf, size - 1);
    close(fd);
    if (n < 0) {
        buf[0] = '\0';
        return -1;
    }
    buf[n] = '\0';
    return (int)n;
}

static void ffTrimRight(char *s)
{
    size_t len = strlen(s);
    while (len > 0 &&
           (s[len - 1] == '\n' || s[len - 1] == '\r' ||
            s[len - 1] == ' ' || s[len - 1] == '\t')) {
        s[--len] = '\0';
    }
}

static const char *ffSkipSpaces(const char *s)
{
    while (*s == ' ' || *s == '\t') {
        s++;
    }
    return s;
}

static bool ffExtractKeyValue(const char *text, const char *key, char sep,
                              char *out, size_t outSize)
{
    size_t keyLen = strlen(key);
    const char *line = text;
    if (!out || outSize == 0) {
        return false;
    }
    while (*line) {
        const char *end = strchr(line, '\n');
        size_t lineLen = end ? (size_t)(end - line) : strlen(line);
        if (lineLen >= keyLen + 1 &&
            strncmp(line, key, keyLen) == 0 &&
            line[keyLen] == sep) {
            const char *value = ffSkipSpaces(line + keyLen + 1);
            size_t len = lineLen - (size_t)(value - line);
            if (*value == '"' && len > 1) {
                value++;
                len--;
                const char *quote = strchr(value, '"');
                if (quote && (size_t)(quote - value) < len) {
                    len = (size_t)(quote - value);
                }
            }
            if (len >= outSize) {
                len = outSize - 1;
            }
            memcpy(out, value, len);
            out[len] = '\0';
            ffTrimRight(out);
            return true;
        }
        if (!end) {
            break;
        }
        line = end + 1;
    }
    out[0] = '\0';
    return false;
}

static unsigned long long ffExtractKib(const char *text, const char *key)
{
    char value[64];
    if (!ffExtractKeyValue(text, key, ':', value, sizeof(value))) {
        return 0;
    }
    return (unsigned long long)atoll(value);
}

static void ffFormatKib(unsigned long long kib, char *out, size_t size)
{
    if (kib >= 1024ull * 1024ull) {
        unsigned long long whole = kib / (1024ull * 1024ull);
        unsigned long long frac = ((kib % (1024ull * 1024ull)) * 100ull) /
                                  (1024ull * 1024ull);
        snprintf(out, size, "%llu.%02llu GiB", whole, frac);
    } else if (kib >= 1024ull) {
        unsigned long long whole = kib / 1024ull;
        unsigned long long frac = ((kib % 1024ull) * 100ull) / 1024ull;
        snprintf(out, size, "%llu.%02llu MiB", whole, frac);
    } else {
        snprintf(out, size, "%llu KiB", kib);
    }
}

static void ffFormatUptime(unsigned long long seconds, char *out, size_t size)
{
    unsigned long long days = seconds / 86400ull;
    unsigned long long hours = (seconds % 86400ull) / 3600ull;
    unsigned long long minutes = (seconds % 3600ull) / 60ull;
    if (days) {
        snprintf(out, size, "%llud %lluh %llum", days, hours, minutes);
    } else if (hours) {
        snprintf(out, size, "%lluh %llum", hours, minutes);
    } else {
        snprintf(out, size, "%llum", minutes);
    }
}

static FFModuleResult *ffModuleAdd(FFContext *ctx, const char *name)
{
    if (ctx->moduleCount >= FF_MAX_MODULES) {
        return NULL;
    }
    FFModuleResult *module = &ctx->modules[ctx->moduleCount++];
    module->name = name;
    module->ok = false;
    ffStrbufClear(&module->value);
    return module;
}

static void ffDetectTitle(FFModuleResult *module)
{
    char host[128];
    char user[64];
    if (ffReadFileBuffer("/etc/hostname", host, sizeof(host)) < 0) {
        strcpy(host, "tiramisu");
    }
    ffTrimRight(host);
    if (getuid() == 0) {
        strcpy(user, "root");
    } else {
        snprintf(user, sizeof(user), "uid%d", getuid());
    }
    ffStrbufSetF(&module->value, "%s@%s", user, host[0] ? host : "tiramisu");
    module->ok = true;
}

static void ffDetectSeparator(FFModuleResult *module)
{
    ffStrbufSetS(&module->value, "--------");
    module->ok = true;
}

static void ffDetectOS(FFModuleResult *module)
{
    char osr[1024];
    char value[128];
    if (ffReadFileBuffer("/etc/os-release", osr, sizeof(osr)) >= 0 &&
        ffExtractKeyValue(osr, "PRETTY_NAME", '=', value, sizeof(value))) {
        ffStrbufSetS(&module->value, value);
    } else {
        ffStrbufSetS(&module->value, "Tiramisu OS");
    }
    module->ok = true;
}

static void ffDetectKernel(FFModuleResult *module)
{
    char value[256];
    if (ffReadFileBuffer("/proc/version", value, sizeof(value)) >= 0) {
        ffTrimRight(value);
        ffStrbufSetS(&module->value, value);
        module->ok = true;
    }
}

static void ffDetectHost(FFModuleResult *module)
{
    char value[128];
    if (ffReadFileBuffer("/etc/hostname", value, sizeof(value)) >= 0) {
        ffTrimRight(value);
        ffStrbufSetS(&module->value, value[0] ? value : "tiramisu");
    } else {
        ffStrbufSetS(&module->value, "tiramisu");
    }
    module->ok = true;
}

static void ffDetectShell(FFModuleResult *module)
{
    const char *shell = getenv("SHELL");
    ffStrbufSetS(&module->value, shell && shell[0] ? shell : "/bin/tsh");
    module->ok = true;
}

static void ffDetectCPU(FFModuleResult *module)
{
    char cpuinfo[1024];
    char value[192];
    if (ffReadFileBuffer("/proc/cpuinfo", cpuinfo, sizeof(cpuinfo)) >= 0 &&
        ffExtractKeyValue(cpuinfo, "model name", ':', value, sizeof(value))) {
        ffStrbufSetS(&module->value, value);
    } else {
        ffStrbufSetS(&module->value, "x86_64");
    }
    module->ok = true;
}

static void ffDetectMemory(FFModuleResult *module)
{
    char meminfo[1024];
    char used[64];
    char total[64];
    if (ffReadFileBuffer("/proc/meminfo", meminfo, sizeof(meminfo)) < 0) {
        return;
    }
    unsigned long long memTotal = ffExtractKib(meminfo, "MemTotal");
    unsigned long long memUsable = ffExtractKib(meminfo, "MemUsable");
    unsigned long long memUsed = memTotal > memUsable ? memTotal - memUsable : 0;
    if (!memTotal) {
        return;
    }
    ffFormatKib(memUsed, used, sizeof(used));
    ffFormatKib(memTotal, total, sizeof(total));
    ffStrbufSetF(&module->value, "%s / %s", used, total);
    module->ok = true;
}

static void ffDetectUptime(FFModuleResult *module)
{
    char text[64];
    char out[64];
    if (ffReadFileBuffer("/proc/uptime", text, sizeof(text)) < 0) {
        return;
    }
    ffFormatUptime((unsigned long long)atoll(text), out, sizeof(out));
    ffStrbufSetS(&module->value, out);
    module->ok = true;
}

static void ffDetectDisplay(FFModuleResult *module)
{
    char fb[1024];
    char type[64];
    char width[32];
    char height[32];
    if (ffReadFileBuffer("/proc/framebuffer", fb, sizeof(fb)) < 0) {
        return;
    }
    ffExtractKeyValue(fb, "type", ':', type, sizeof(type));
    ffExtractKeyValue(fb, "width", ':', width, sizeof(width));
    ffExtractKeyValue(fb, "height", ':', height, sizeof(height));
    if (width[0] && height[0]) {
        ffStrbufSetF(&module->value, "%sx%s (%s)", width, height,
                     type[0] ? type : "framebuffer");
    } else {
        ffStrbufSetS(&module->value, type[0] ? type : "console");
    }
    module->ok = true;
}

static void ffDetectTerminal(FFModuleResult *module)
{
    struct syscall_winsize ws;
    int fd = open("/dev/tty", O_RDONLY, 0);
    if (fd >= 0 && ioctl(fd, TNU_IOCTL_TIOCGWINSZ, &ws) == 0) {
        close(fd);
        ffStrbufSetF(&module->value, "tnu-vt (%ux%u)", ws.ws_col, ws.ws_row);
    } else {
        if (fd >= 0) {
            close(fd);
        }
        ffStrbufSetS(&module->value, "tnu-vt");
    }
    module->ok = true;
}

static void ffDetectLocalIP(FFModuleResult *module)
{
    char dev[2048];
    if (ffReadFileBuffer("/proc/net/dev", dev, sizeof(dev)) < 0) {
        return;
    }
    const char *line = strchr(dev, '\n');
    while (line && *line) {
        line++;
        if (strstr(line, "\t") && !strstr(line, "\t-\t")) {
            char copy[256];
            const char *end = strchr(line, '\n');
            size_t len = end ? (size_t)(end - line) : strlen(line);
            if (len >= sizeof(copy)) {
                len = sizeof(copy) - 1;
            }
            memcpy(copy, line, len);
            copy[len] = '\0';
            ffStrbufSetS(&module->value, copy);
            module->ok = true;
            return;
        }
        line = strchr(line, '\n');
    }
}

static void ffDetectColors(FFModuleResult *module)
{
    ffStrbufSetS(&module->value,
                 "\x1b[40m  \x1b[41m  \x1b[42m  \x1b[43m  "
                 "\x1b[44m  \x1b[45m  \x1b[46m  \x1b[47m  \x1b[0m");
    module->ok = true;
}

static void ffDetectPort(FFModuleResult *module)
{
    ffStrbufSetF(&module->value, "%s native backend (%s)",
                 FASTFETCH_TNU_PORT_VERSION, FASTFETCH_UPSTREAM_URL);
    module->ok = true;
}

typedef void (*FFDetector)(FFModuleResult *);

typedef struct FFModuleDef {
    const char *name;
    FFDetector detect;
} FFModuleDef;

static const FFModuleDef ffModuleDefs[] = {
    { "title", ffDetectTitle },
    { "separator", ffDetectSeparator },
    { "os", ffDetectOS },
    { "kernel", ffDetectKernel },
    { "host", ffDetectHost },
    { "shell", ffDetectShell },
    { "cpu", ffDetectCPU },
    { "memory", ffDetectMemory },
    { "uptime", ffDetectUptime },
    { "display", ffDetectDisplay },
    { "terminal", ffDetectTerminal },
    { "localip", ffDetectLocalIP },
    { "colors", ffDetectColors },
    { "port", ffDetectPort },
};

static const FFModuleDef *ffFindModule(const char *name, size_t len)
{
    for (size_t i = 0; i < sizeof(ffModuleDefs) / sizeof(ffModuleDefs[0]); i++) {
        if (strlen(ffModuleDefs[i].name) == len &&
            strncasecmp(ffModuleDefs[i].name, name, len) == 0) {
            return &ffModuleDefs[i];
        }
    }
    return NULL;
}

static void ffCollectModule(FFContext *ctx, const FFModuleDef *def)
{
    FFModuleResult *module = ffModuleAdd(ctx, def->name);
    if (!module) {
        return;
    }
    def->detect(module);
}

static void ffCollectStructure(FFContext *ctx)
{
    const char *structure = ctx->options.structure;
    if (!structure || !structure[0]) {
        structure = "title:separator:os:kernel:host:uptime:shell:display:cpu:memory:terminal:localip:colors";
    }
    const char *start = structure;
    while (*start) {
        const char *end = strchr(start, ':');
        size_t len = end ? (size_t)(end - start) : strlen(start);
        if (len > 0) {
            const FFModuleDef *def = ffFindModule(start, len);
            if (def) {
                ffCollectModule(ctx, def);
            }
        }
        if (!end) {
            break;
        }
        start = end + 1;
    }
}

static const char *ffLogoBuiltin[] = {
    "  _______ _                                ",
    " |_   _(_) |                               ",
    "   | |  _| |_ __ _ _ __ ___  _ ___ _   _  ",
    "   | | | | __/ _` | '_ ` _ \\| / __| | | | ",
    "   | | | | || (_| | | | | | | \\__ \\ |_| | ",
    "   |_| |_|\\__\\__,_|_| |_| |_|_|___/\\__,_| ",
    NULL,
};

static size_t ffLogoLoad(const char **lines, char *storage, size_t storageSize)
{
    int n = ffReadFileBuffer("/etc/fastfetch-logo", storage, storageSize);
    if (n < 0) {
        n = ffReadFileBuffer("/etc/sysfetch-logo", storage, storageSize);
    }
    if (n <= 0) {
        size_t count = 0;
        while (ffLogoBuiltin[count] && count < FF_MAX_LOGO_LINES) {
            lines[count] = ffLogoBuiltin[count];
            count++;
        }
        return count;
    }
    size_t count = 0;
    char *p = storage;
    while (*p && count < FF_MAX_LOGO_LINES) {
        lines[count++] = p;
        char *nl = strchr(p, '\n');
        if (!nl) {
            break;
        }
        *nl = '\0';
        p = nl + 1;
    }
    return count;
}

static void ffPrintJsonEscaped(const char *s)
{
    putchar('"');
    while (*s) {
        if (*s == '"' || *s == '\\') {
            putchar('\\');
            putchar(*s);
        } else if (*s == '\n') {
            fputs("\\n", stdout);
        } else {
            putchar(*s);
        }
        s++;
    }
    putchar('"');
}

static void ffPrintJson(const FFContext *ctx)
{
    puts("[");
    for (size_t i = 0; i < ctx->moduleCount; i++) {
        const FFModuleResult *module = &ctx->modules[i];
        printf("  {\"type\":");
        ffPrintJsonEscaped(module->name);
        printf(",\"result\":");
        ffPrintJsonEscaped(module->ok ? module->value.chars : "");
        printf("}%s\n", i + 1 < ctx->moduleCount ? "," : "");
    }
    puts("]");
}

static void ffPrintLogoAndModules(const FFContext *ctx)
{
    const char *logoLines[FF_MAX_LOGO_LINES];
    char logoStorage[FF_MAX_LOGO_TEXT];
    size_t logoCount = ctx->options.showLogo ?
                       ffLogoLoad(logoLines, logoStorage, sizeof(logoStorage)) : 0;
    size_t logoWidth = 0;
    for (size_t i = 0; i < logoCount; i++) {
        size_t len = strlen(logoLines[i]);
        if (len > logoWidth) {
            logoWidth = len;
        }
    }

    size_t rows = logoCount > ctx->moduleCount ? logoCount : ctx->moduleCount;
    for (size_t row = 0; row < rows; row++) {
        if (logoCount) {
            if (row < logoCount) {
                if (!ctx->options.pipe) {
                    fputs(FF_COLOR_LOGO, stdout);
                }
                fputs(logoLines[row], stdout);
                if (!ctx->options.pipe) {
                    fputs(FF_COLOR_RESET, stdout);
                }
                for (size_t p = strlen(logoLines[row]); p < logoWidth; p++) {
                    putchar(' ');
                }
            } else {
                for (size_t p = 0; p < logoWidth; p++) {
                    putchar(' ');
                }
            }
            fputs("  ", stdout);
        }
        if (row < ctx->moduleCount) {
            const FFModuleResult *module = &ctx->modules[row];
            if (module->ok) {
                if (!ctx->options.pipe && strcmp(module->name, "title") == 0) {
                    fputs(FF_COLOR_TITLE, stdout);
                } else if (!ctx->options.pipe) {
                    fputs(FF_COLOR_KEY, stdout);
                }
                printf("%-10s", module->name);
                if (!ctx->options.pipe) {
                    fputs(FF_COLOR_RESET FF_COLOR_VALUE, stdout);
                }
                printf(": %s", module->value.chars);
                if (!ctx->options.pipe) {
                    fputs(FF_COLOR_RESET, stdout);
                }
            }
        }
        putchar('\n');
    }
}

static void ffPrintHelp(void)
{
    puts("Fastfetch TNU port");
    puts("Usage: fastfetch [OPTIONS]");
    puts("  -s, --structure LIST     Colon-separated module list");
    puts("      --format json        Print JSON module results");
    puts("      --logo none          Disable logo");
    puts("      --pipe false|true    Disable or enable color-aware terminal output");
    puts("      --version            Show version and upstream provenance");
    puts("      --help               Show this help");
    puts("");
    puts("Modules: title separator os kernel host shell cpu memory uptime display terminal localip colors port");
}

static bool ffParseBool(const char *s, bool fallback)
{
    if (!s) {
        return fallback;
    }
    if (strcmp(s, "true") == 0 || strcmp(s, "1") == 0 || strcmp(s, "yes") == 0) {
        return true;
    }
    if (strcmp(s, "false") == 0 || strcmp(s, "0") == 0 || strcmp(s, "no") == 0) {
        return false;
    }
    return fallback;
}

int main(int argc, char **argv)
{
    FFContext ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.options.showLogo = true;
    ctx.options.showColors = true;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            ffPrintHelp();
            return 0;
        }
        if (strcmp(argv[i], "--version") == 0) {
            printf("%s %s (%s)\n", FASTFETCH_PROJECT_NAME,
                   FASTFETCH_TNU_PORT_VERSION, FASTFETCH_UPSTREAM_URL);
            return 0;
        }
        if ((strcmp(argv[i], "-s") == 0 || strcmp(argv[i], "--structure") == 0) &&
            i + 1 < argc) {
            ctx.options.structure = argv[++i];
            continue;
        }
        if (strcmp(argv[i], "--format") == 0 && i + 1 < argc) {
            const char *fmt = argv[++i];
            if (strcmp(fmt, "json") == 0) {
                ctx.options.json = true;
                ctx.options.pipe = true;
                ctx.options.showLogo = false;
            }
            continue;
        }
        if (strcmp(argv[i], "--logo") == 0 && i + 1 < argc) {
            const char *logo = argv[++i];
            if (strcmp(logo, "none") == 0) {
                ctx.options.showLogo = false;
            }
            continue;
        }
        if (strcmp(argv[i], "--pipe") == 0 && i + 1 < argc) {
            ctx.options.pipe = ffParseBool(argv[++i], ctx.options.pipe);
            continue;
        }
        if (strncmp(argv[i], "--structure=", 12) == 0) {
            ctx.options.structure = argv[i] + 12;
            continue;
        }
        if (strncmp(argv[i], "--format=", 9) == 0) {
            if (strcmp(argv[i] + 9, "json") == 0) {
                ctx.options.json = true;
                ctx.options.pipe = true;
                ctx.options.showLogo = false;
            }
            continue;
        }
    }

    ffCollectStructure(&ctx);
    if (ctx.options.json) {
        ffPrintJson(&ctx);
    } else {
        ffPrintLogoAndModules(&ctx);
    }
    return 0;
}
