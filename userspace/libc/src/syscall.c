#include <tnu/syscall.h>
#include <stdarg.h>

long tnu_syscall(long n, long a0, long a1, long a2, long a3, long a4, long a5)
{
    long ret;
    register long r10 __asm__("r10") = a3;
    register long r8 __asm__("r8") = a4;
    register long r9 __asm__("r9") = a5;
    __asm__ volatile("syscall"
                     : "=a"(ret)
                     : "a"(n), "D"(a0), "S"(a1), "d"(a2), "r"(r10), "r"(r8), "r"(r9)
                     : "rcx", "r11", "memory");
    return ret;
}

ssize_t read(int fd, void *buf, size_t count)
{
    return tnu_syscall(SYS_READ, fd, (long)buf, (long)count, 0, 0, 0);
}

ssize_t write(int fd, const void *buf, size_t count)
{
    return tnu_syscall(SYS_WRITE, fd, (long)buf, (long)count, 0, 0, 0);
}

int open(const char *path, int flags, ...)
{
    int mode = 0;
    if (flags & O_CREAT) {
        va_list ap;
        va_start(ap, flags);
        mode = va_arg(ap, int);
        va_end(ap);
    }
    return (int)tnu_syscall(SYS_OPEN, (long)path, flags, mode, 0, 0, 0);
}

int close(int fd)
{
    return (int)tnu_syscall(SYS_CLOSE, fd, 0, 0, 0, 0, 0);
}

int spawn(const char *path)
{
    return (int)tnu_syscall(SYS_SPAWN, (long)path, 0, 0, 0, 0, 0);
}

int exec(const char *path)
{
    return (int)tnu_syscall(SYS_EXEC, (long)path, 0, 0, 0, 0, 0);
}

int wait(int pid)
{
    return (int)tnu_syscall(SYS_WAIT, pid, 0, 0, 0, 0, 0);
}

void exit(int code)
{
    tnu_syscall(SYS_EXIT, code, 0, 0, 0, 0, 0);
    for (;;) {
    }
}

int getpid(void)
{
    return (int)tnu_syscall(SYS_GETPID, 0, 0, 0, 0, 0, 0);
}

int getppid(void)
{
    return (int)tnu_syscall(SYS_GETPPID, 0, 0, 0, 0, 0, 0);
}

int getuid(void)
{
    return (int)tnu_syscall(SYS_GETUID, 0, 0, 0, 0, 0, 0);
}

int getgid(void)
{
    return (int)tnu_syscall(SYS_GETGID, 0, 0, 0, 0, 0, 0);
}

int chdir(const char *path)
{
    return (int)tnu_syscall(SYS_CHDIR, (long)path, 0, 0, 0, 0, 0);
}

int getcwd(char *buf, size_t size)
{
    return (int)tnu_syscall(SYS_GETCWD, (long)buf, (long)size, 0, 0, 0, 0);
}

int mkdir(const char *path, mode_t mode)
{
    return (int)tnu_syscall(SYS_MKDIR, (long)path, mode, 0, 0, 0, 0);
}

int unlink(const char *path)
{
    return (int)tnu_syscall(SYS_UNLINK, (long)path, 0, 0, 0, 0, 0);
}

int stat(const char *path, struct stat *st)
{
    return (int)tnu_syscall(SYS_STAT, (long)path, (long)st, 0, 0, 0, 0);
}

int chmod(const char *path, mode_t mode)
{
    return (int)tnu_syscall(SYS_CHMOD, (long)path, mode, 0, 0, 0, 0);
}

int chown(const char *path, uid_t uid, gid_t gid)
{
    return (int)tnu_syscall(SYS_CHOWN, (long)path, uid, gid, 0, 0, 0);
}

off_t lseek(int fd, off_t offset, int whence)
{
    return (off_t)tnu_syscall(SYS_LSEEK, fd, (long)offset, whence, 0, 0, 0);
}

int access(const char *path, int mode)
{
    return (int)tnu_syscall(SYS_ACCESS, (long)path, mode, 0, 0, 0, 0);
}

int dup(int oldfd)
{
    return (int)tnu_syscall(SYS_DUP, oldfd, 0, 0, 0, 0, 0);
}

int dup2(int oldfd, int newfd)
{
    return (int)tnu_syscall(SYS_DUP2, oldfd, newfd, 0, 0, 0, 0);
}
