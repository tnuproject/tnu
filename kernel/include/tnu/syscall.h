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
    SYS_FSTAT = 28,
    SYS_NANOSLEEP = 29,
    SYS_POLL = 30,
    /* Block device syscalls for disk I/O */
    SYS_BLOCK_GET_COUNT = 31,
    SYS_BLOCK_GET_INFO = 32,
    SYS_BLOCK_READ = 33,
    SYS_BLOCK_WRITE = 34,
    SYS_BLOCK_SYNC = 35,
    SYS_LOGIN = 36,
    /* Flush the persistent TFS image to disk immediately. */
    SYS_SYNC  = 37,
    /* Memory-map device (e.g. framebuffer) into user address space. */
    SYS_MMAP  = 38,
    /* POSIX select / pselect6 — used by nano, editors, networking. */
    SYS_SELECT  = 39,
    SYS_PSELECT = 40,
    /* Wi-Fi control plane for scan/connect/status. */
    SYS_WIFI_SCAN = 41,
    SYS_WIFI_CONNECT = 42,
    SYS_WIFI_STATUS = 43,
    /* Power management */
    SYS_SHUTDOWN = 44,
    SYS_REBOOT = 45,
    SYS_SET_PASSWORD = 46,
    SYS_ADD_USER = 47,
    SYS_DEL_USER = 48,
    /* Network syscalls */
    SYS_RESOLVE4 = 49,
    SYS_SOCKET = 50,
    SYS_CONNECT = 51,
    SYS_SEND = 52,
    SYS_RECV = 53,
    SYS_WIFI_AUTOCONNECT = 54,
    SYS_WIFI_DISCONNECT = 55,
    SYS_WIFI_START = 56,
};

#define TNU_IOCTL_FB_GETINFO 0x544e4601u
#define TNU_IOCTL_TTY_GETSIZE 0x544e5401u
#define TNU_IOCTL_TIOCGWINSZ 0x5413u
/* Standard Linux termios ioctl numbers (used by musl/glibc ABI) */
#define TNU_IOCTL_TCGETS  0x5401u   /* TCGETS  — get termios */
#define TNU_IOCTL_TCSETS  0x5402u   /* TCSETS  — set termios (immediate) */
#define TNU_IOCTL_TCSETSW 0x5403u   /* TCSETSW — set termios (after drain) */
#define TNU_IOCTL_TCSETSF 0x5404u   /* TCSETSF — set termios (after flush) */

/* Minimal termios flags needed by the kernel TTY layer */
#define TNU_TTYF_ICANON  0x0002u   /* canonical (line-buffered) mode */
#define TNU_TTYF_ECHO    0x0008u   /* echo input */
#define TNU_TTYF_ISIG    0x0001u   /* generate signals */

struct syscall_termios {
    uint32_t c_iflag;
    uint32_t c_oflag;
    uint32_t c_cflag;
    uint32_t c_lflag;
    uint8_t  c_cc[32]; /* index 5 = VTIME, 6 = VMIN */
};

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
