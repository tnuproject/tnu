#ifndef TNU_SYSCALL_H
#define TNU_SYSCALL_H

#include <tnu/types.h>

enum syscall_number {
    SYS_READ = 0,
    SYS_WRITE = 1,
    SYS_OPEN = 2,
    SYS_CLOSE = 3,
    SYS_SPAWN = 4,
    SYS_EXEC = 5,
    SYS_WAIT = 6,
    SYS_EXIT = 7,
    SYS_GETPID = 8,
    SYS_CHDIR = 9,
    SYS_GETCWD = 10,
    SYS_MKDIR = 11,
    SYS_UNLINK = 12,
    SYS_STAT = 13,
    SYS_CHMOD = 14,
    SYS_CHOWN = 15,
    SYS_GETUID = 16,
    SYS_GETGID = 17,
    SYS_LSEEK = 18,
    SYS_ACCESS = 19,
    SYS_DUP = 20,
    SYS_DUP2 = 21,
    SYS_GETPPID = 22,
    SYS_READDIR = 23,
    SYS_IOCTL = 24,
    SYS_UPTIME_MS = 25,
    SYS_BRK = 26,
    SYS_SIGACTION = 27,
};

#define TNU_IOCTL_FB_GETINFO 0x544e4601u
#define TNU_IOCTL_TTY_GETSIZE 0x544e5401u
#define TNU_IOCTL_TIOCGWINSZ 0x5413u

struct syscall_dirent {
    uint64_t d_ino;
    unsigned char d_type;
    char d_name[256];
};

struct syscall_fb_info {
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint8_t bpp;
};

struct syscall_winsize {
    uint16_t ws_row;
    uint16_t ws_col;
    uint16_t ws_xpixel;
    uint16_t ws_ypixel;
};

long syscall_dispatch(uint64_t number, uint64_t a0, uint64_t a1, uint64_t a2,
                      uint64_t a3, uint64_t a4, uint64_t a5);

#endif
