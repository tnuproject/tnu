#ifndef TNU_SYS_IOCTL_H
#define TNU_SYS_IOCTL_H

#include <tnu/syscall.h>

#define TIOCLINUX 0x541c
#define TIOCGWINSZ TNU_IOCTL_TIOCGWINSZ

struct winsize {
    unsigned short ws_row;
    unsigned short ws_col;
    unsigned short ws_xpixel;
    unsigned short ws_ypixel;
};

int ioctl(int fd, unsigned long request, ...);

#endif
