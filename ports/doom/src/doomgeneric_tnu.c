/*
 * doomgeneric_tnu.c — TNU platform backend for doomgeneric
 *
 * Based on doomgeneric_linuxvt.c by Techflash (GPL-2.0+)
 * Adapted for TNU: uses /dev/fb0 (write-only, TNU ioctl) and /dev/input/kbd
 */

#include "doomgeneric.h"
#include "doomkeys.h"
#include "i_system.h"

#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <tnu/syscall.h>

/* ------------------------------------------------------------------ */
/* TNU framebuffer                                                      */
/* ------------------------------------------------------------------ */

static int   fb_fd   = -1;
static int   kbd_fd  = -1;
static int   fb_w    = 0;
static int   fb_h    = 0;
static uint32_t *scale_buf = NULL;
static uint32_t *fb_mmap = NULL;  /* mmap'd framebuffer for zero-copy */

/* ------------------------------------------------------------------ */
/* Keyboard                                                             */
/* ------------------------------------------------------------------ */

#define KEYQUEUE_SIZE 64

/* TNU raw key codes from keyboard.h (must match kernel values) */
#define TNU_KEY_UP    0x101
#define TNU_KEY_DOWN  0x102
#define TNU_KEY_LEFT  0x103
#define TNU_KEY_RIGHT 0x104
#define TNU_KEY_HOME  0x105
#define TNU_KEY_END   0x106
#define TNU_KEY_DEL   0x107
#define TNU_KEY_CTRL_C 0x108

static unsigned short s_KeyQueue[KEYQUEUE_SIZE];
static unsigned int   s_KeyQueueWrite = 0;
static unsigned int   s_KeyQueueRead  = 0;

static void addKeyToQueue(int pressed, unsigned char doomKey)
{
    /* Prevent overflow of the fixed-size circular queue. If the queue is full
     * we simply drop the new key event – this is better than overwriting older
     * unprocessed events, which can corrupt the input state and cause the game
     * to stop responding to further keys.
     */
    if ((s_KeyQueueWrite - s_KeyQueueRead) >= KEYQUEUE_SIZE) {
        return; // queue full, drop event
    }
    unsigned short entry = (unsigned short)((pressed << 8) | doomKey);
    s_KeyQueue[s_KeyQueueWrite % KEYQUEUE_SIZE] = entry;
    s_KeyQueueWrite++;
}

static unsigned char tnu_to_doom(uint16_t k, int *valid)
{
    *valid = 1;
    /* TNU special keys */
    if (k == TNU_KEY_UP)    return KEY_UPARROW;
    if (k == TNU_KEY_DOWN)  return KEY_DOWNARROW;
    if (k == TNU_KEY_LEFT)  return KEY_LEFTARROW;
    if (k == TNU_KEY_RIGHT) return KEY_RIGHTARROW;
    if (k == TNU_KEY_HOME)  return KEY_HOME;
    if (k == TNU_KEY_END)   return KEY_END;
    if (k == TNU_KEY_DEL)   return KEY_DEL;
    if (k == TNU_KEY_CTRL_C) return KEY_ESCAPE;
    /* Additional TNU keys for Doom - 0x113=SPACE, 0x114=CTRL, 0x115=ALT */
    if (k == 0x113) return ' ';           /* SPACE = fire (ASCII space) */
    if (k == 0x114) return KEY_FIRE;      /* CTRL = fire */
    if (k == 0x115) return KEY_USE;       /* ALT = use */

    /* ASCII range */
    if (k < 0x100) {
        unsigned char c = (unsigned char)k;
        if (c == '\r' || c == '\n') return KEY_ENTER;
        if (c == 27)                return KEY_ESCAPE;
        if (c == ' ')               return KEY_USE;
        if (c == 8 || c == 127)     return KEY_BACKSPACE;
        if (c == '\t')              return KEY_TAB;
        /* ctrl+letters */
        if (c >= 1 && c <= 26)      return (unsigned char)('a' + c - 1);
        if (c >= 'A' && c <= 'Z')   return (unsigned char)(c + 32);
        if (c >= 32 && c < 127)     return c;
    }

    *valid = 0;
    return 0;
}

static void checkKeys(void)
{
    if (kbd_fd < 0) return;

    /* /dev/input/kbd returns uint16_t TNU key events.
       Bit 15 set = release (key-up), clear = press (key-down). */
    uint16_t buf[32];
    ssize_t n = read(kbd_fd, buf, sizeof(buf));
    if (n <= 0) return;

    int count = (int)(n / (ssize_t)sizeof(uint16_t));
    for (int i = 0; i < count; i++) {
        uint16_t raw = buf[i];
        int released = (raw & 0x8000) != 0;
        uint16_t code = raw & 0x7fff;
        int valid;
        unsigned char dk = tnu_to_doom(code, &valid);
        if (valid) {
            addKeyToQueue(released ? 0 : 1, dk);
        }
    }
}

/* ------------------------------------------------------------------ */
/* DG_* API                                                             */
/* ------------------------------------------------------------------ */

void DG_Init(void)
{
    fb_fd  = open("/dev/fb0", O_RDWR);
    /* Open keyboard in NON-BLOCKING mode to prevent hangs in checkKeys() */
    kbd_fd = open("/dev/input/kbd", O_RDONLY | O_NONBLOCK);

    if (fb_fd >= 0) {
        struct syscall_fb_info info;
        int ret = ioctl(fb_fd, TNU_IOCTL_FB_GETINFO, &info);
        if (ret == 0 && info.width > 0 && info.height > 0) {
            fb_w = (int)info.width;
            fb_h = (int)info.height;

            /* Try to mmap the framebuffer for zero-copy rendering */
            size_t fb_size = (size_t)info.pitch * info.height;
            fb_mmap = mmap(NULL, fb_size, PROT_READ | PROT_WRITE, MAP_SHARED, fb_fd, 0);
            if (fb_mmap == MAP_FAILED || fb_mmap == NULL) {
                fb_mmap = NULL;
                fprintf(stderr, "doom: mmap /dev/fb0 failed, using write() fallback\n");
            }
        }
    }
    
    /* Safety bounds - prevent infinite loops and invalid values */
    if (fb_w <= 0 || fb_w > 4096 || fb_h <= 0 || fb_h > 4096) {
        fb_w = DOOMGENERIC_RESX;
        fb_h = DOOMGENERIC_RESY;
    }

    if (fb_w != DOOMGENERIC_RESX || fb_h != DOOMGENERIC_RESY) {
        scale_buf = malloc((size_t)fb_w * (size_t)fb_h * sizeof(uint32_t));
        if (!scale_buf) {
            /* Fall back to native resolution */
            fb_w = DOOMGENERIC_RESX;
            fb_h = DOOMGENERIC_RESY;
        }
    }

    if (kbd_fd < 0)
        fprintf(stderr, "doom: warning: /dev/input/kbd not available\n");
}

void DG_DrawFrame(void)
{
    if (fb_fd < 0) return;

    uint32_t *src = DG_ScreenBuffer;

    /* Safety check for invalid dimensions */
    if (fb_w <= 0 || fb_h <= 0) {
        fb_w = DOOMGENERIC_RESX;
        fb_h = DOOMGENERIC_RESY;
    }

    /* Fast path: mmap'd framebuffer — direct memcpy/scaling to mapped memory */
    if (fb_mmap) {
        if (!scale_buf) {
            /* 1:1 native resolution — direct memcpy to framebuffer */
            memcpy(fb_mmap, src, (size_t)(DOOMGENERIC_RESX * DOOMGENERIC_RESY) * sizeof(uint32_t));
        } else {
            /* Nearest-neighbour upscale directly into mmap'd framebuffer */
            for (int dy = 0; dy < fb_h && dy < 4096; dy++) {
                int sy = dy * DOOMGENERIC_RESY / fb_h;
                if (sy >= DOOMGENERIC_RESY) continue;
                const uint32_t *srow = src + sy * DOOMGENERIC_RESX;
                uint32_t *drow = fb_mmap + dy * fb_w;
                for (int dx = 0; dx < fb_w && dx < 4096; dx++) {
                    int sx = dx * DOOMGENERIC_RESX / fb_w;
                    if (sx < DOOMGENERIC_RESX)
                        drow[dx] = srow[sx];
                }
            }
        }
    } else {
        /* Fallback: write() syscall path */
        if (!scale_buf) {
            write(fb_fd, src,
                  (size_t)(DOOMGENERIC_RESX * DOOMGENERIC_RESY) * sizeof(uint32_t));
        } else {
            /* Nearest-neighbour upscale with bounds check */
            for (int dy = 0; dy < fb_h && dy < 4096; dy++) {
                int sy = dy * DOOMGENERIC_RESY / fb_h;
                if (sy >= DOOMGENERIC_RESY) continue;
                const uint32_t *srow = src + sy * DOOMGENERIC_RESX;
                uint32_t *drow = scale_buf + dy * fb_w;
                for (int dx = 0; dx < fb_w && dx < 4096; dx++) {
                    int sx = dx * DOOMGENERIC_RESX / fb_w;
                    if (sx < DOOMGENERIC_RESX)
                        drow[dx] = srow[sx];
                }
            }
            write(fb_fd, scale_buf,
                  (size_t)fb_w * (size_t)fb_h * sizeof(uint32_t));
        }
    }

    checkKeys();
}

void DG_SleepMs(uint32_t ms)
{
    if (ms == 0) return;

    /* Use nanosleep syscall if available (preferred), otherwise fall back to
     * a bounded busy-wait with yield to prevent infinite loops on systems
     * where uptime_ms might be stuck at 0 or overflow. */
    struct {
        int64_t tv_sec;
        int64_t tv_nsec;
    } req;
    req.tv_sec = (int64_t)(ms / 1000);
    req.tv_nsec = (int64_t)((ms % 1000) * 1000000);
    
    /* Try nanosleep first (SYS_NANOSLEEP = 29) */
    long ret = tnu_syscall(29, (long)&req, 0, 0, 0, 0, 0);
    
    /* Drain keyboard buffer during sleep to prevent event overflow.
     * This fixes the bug where doom stops receiving input after a few minutes
     * because the kernel's 1024-event ring buffer fills up during cutscenes/menus. */
    checkKeys();
    
    if (ret == 0) return;

    /* Fallback: bounded busy-wait with early exit if uptime doesn't advance.
     * This prevents infinite loops when uptime_ms is broken or stuck at 0. */
    uint64_t start = (uint64_t)tnu_syscall(SYS_UPTIME_MS, 0, 0, 0, 0, 0, 0);
    if (start == 0) {
        /* Timer not initialized yet — just yield once and return */
        __asm__ volatile("pause");
        checkKeys();
        return;
    }

    uint64_t end = start + ms;
    uint32_t iterations = 0;
    uint64_t last_time = start;

    while ((uint64_t)tnu_syscall(SYS_UPTIME_MS, 0, 0, 0, 0, 0, 0) < end) {
        __asm__ volatile("pause");
        
        /* Drain keyboard periodically during busy-wait */
        if ((iterations % 10000) == 0) {
            checkKeys();
        }
        
        /* Safety: if we've iterated 100000 times without time advancing,
         * assume the timer is stuck and break out to prevent hang. */
        if (++iterations > 100000) {
            uint64_t now = (uint64_t)tnu_syscall(SYS_UPTIME_MS, 0, 0, 0, 0, 0, 0);
            if (now == last_time) {
                /* Time is stuck — abort sleep to prevent infinite loop */
                checkKeys();
                break;
            }
            last_time = now;
            iterations = 0;
        }
    }
    
    /* Final drain after sleep completes */
    checkKeys();
}

uint32_t DG_GetTicksMs(void)
{
    return (uint32_t)tnu_syscall(SYS_UPTIME_MS, 0, 0, 0, 0, 0, 0);
}

int DG_GetKey(int *pressed, unsigned char *doomKey)
{
    checkKeys();
    if (s_KeyQueueRead == s_KeyQueueWrite) return 0;
    unsigned short entry = s_KeyQueue[s_KeyQueueRead % KEYQUEUE_SIZE];
    s_KeyQueueRead++;
    *pressed = entry >> 8;
    *doomKey  = entry & 0xff;
    return 1;
}

void DG_SetWindowTitle(const char *title)
{
    (void)title;
}

/* ------------------------------------------------------------------ */
/* WAD selection via --version=N                                        */
/* ------------------------------------------------------------------ */

static const char *wad_for_version(int ver)
{
    switch (ver) {
    case 1: return "/usr/share/games/doom/Doom1.WAD";
    case 2: return "/usr/share/games/doom/Doom2.WAD";
    case 3: return "/usr/share/games/doom/Doom3.WAD";
    default: return NULL;
    }
}

static int has_iwad_argument(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-iwad") == 0 || strncmp(argv[i], "-iwad=", 6) == 0) {
            return 1;
        }
    }
    return 0;
}

int main(int argc, char **argv)
{
    /* Scan argv for --version=N or a direct WAD path argument.
     * Usage: doom [/path/to/file.WAD] [-iwad /path/to/file.WAD] [other args]
     * If the first argument (argv[1]) ends with .WAD/.wad and is not preceded by -iwad,
     * treat it as the IWAD path.  Otherwise use the default shareware WAD. */
    const char *wad_path = has_iwad_argument(argc, argv) ? NULL
                                                         : "/usr/share/games/doom/Doom1.WAD";
    static char *new_argv[64];
    int new_argc = 0;

    for (int i = 0; i < argc && new_argc < 62; i++) {
        if (strncmp(argv[i], "--version=", 10) == 0) {
            int ver = atoi(argv[i] + 10);
            wad_path = wad_for_version(ver);
            if (!wad_path) {
                fprintf(stderr, "doom: unknown --version=%d (use 1, 2 or 3)\n", ver);
                return 1;
            }
        } else if (i == 1 && !has_iwad_argument(argc, argv)) {
            /* First arg after program name — check if it's a WAD path */
            size_t len = strlen(argv[i]);
            if (len > 4 && 
                (strcmp(argv[i] + len - 4, ".WAD") == 0 || 
                 strcmp(argv[i] + len - 4, ".wad") == 0)) {
                /* User provided a WAD path directly: doom /path/to/file.WAD */
                wad_path = argv[i];
                /* Don't add this to new_argv — we'll inject it as -iwad below */
                continue;
            }
            new_argv[new_argc++] = argv[i];
        } else {
            new_argv[new_argc++] = argv[i];
        }
    }

    if (wad_path) {
        /* Inject -iwad <path> right after argv[0] */
        if (new_argc + 2 <= 63) {
            /* shift everything after argv[0] up by 2 */
            for (int i = new_argc - 1; i >= 1; i--)
                new_argv[i + 2] = new_argv[i];
            new_argv[1] = (char *)"-iwad";
            new_argv[2] = (char *)wad_path;
            new_argc += 2;
        }
    }

    new_argv[new_argc] = NULL;
    doomgeneric_Create(new_argc, new_argv);
    for (;;)
        doomgeneric_Tick();
    return 0;
}
