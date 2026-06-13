#ifndef TNU_USER_SYSCALL_H
#define TNU_USER_SYSCALL_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

struct stat;

enum {
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
};

#define TNU_IOCTL_FB_GETINFO 0x544e4601u
#define TNU_IOCTL_TTY_GETSIZE 0x544e5401u
#define TNU_IOCTL_TIOCGWINSZ 0x5413u
/* Standard Linux termios ioctl numbers */
#define TNU_IOCTL_TCGETS  0x5401u
#define TNU_IOCTL_TCSETS  0x5402u
#define TNU_IOCTL_TCSETSW 0x5403u
#define TNU_IOCTL_TCSETSF 0x5404u

#define TNU_TTYF_ICANON  0x0002u
#define TNU_TTYF_ECHO    0x0008u
#define TNU_TTYF_ISIG    0x0001u

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

#define O_RDONLY 0x0
#define O_WRONLY 0x1
#define O_RDWR   0x2
#define O_CREAT  0x40
#define O_TRUNC  0x200
#define O_APPEND 0x400
#define O_EXCL   0x800

long tnu_syscall(long n, long a0, long a1, long a2, long a3, long a4, long a5);
ssize_t read(int fd, void *buf, size_t count);
ssize_t write(int fd, const void *buf, size_t count);
int open(const char *path, int flags, ...);
int close(int fd);
int spawn(const char *path);
int exec(const char *path);
int execv(const char *path, char *const argv[]);
int execvp(const char *file, char *const argv[]);
int wait(int pid);
void exit(int code) __attribute__((noreturn));
int getpid(void);
int getppid(void);
int getuid(void);
int getgid(void);
int chdir(const char *path);
int getcwd(char *buf, size_t size);
int mkdir(const char *path, mode_t mode);
int unlink(const char *path);
int stat(const char *path, struct stat *st);
int fstat(int fd, struct stat *st);
int chmod(const char *path, mode_t mode);
int chown(const char *path, uid_t uid, gid_t gid);
off_t lseek(int fd, off_t offset, int whence);
int access(const char *path, int mode);
int dup(int oldfd);
int dup2(int oldfd, int newfd);
int readdir_fd(int fd, struct syscall_dirent *out);
int ioctl(int fd, unsigned long request, ...);
uint64_t uptime_ms(void);
void *sbrk(intptr_t increment);
int brk(void *addr);

#endif
