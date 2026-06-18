#include <tnu/syscall.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

#define ATEXIT_MAX 16

static void (*atexit_handlers[ATEXIT_MAX])(void);
static int atexit_count;

int atexit(void (*function)(void))
{
    if (!function || atexit_count >= ATEXIT_MAX) {
        return -1;
    }
    atexit_handlers[atexit_count++] = function;
    return 0;
}

static void run_atexit_handlers(void)
{
    while (atexit_count > 0) {
        void (*handler)(void) = atexit_handlers[--atexit_count];
        if (handler) {
            handler();
        }
    }
}

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
    char *argv[] = { (char *)path, 0 };
    return execv(path, argv);
}

int execv(const char *path, char *const argv[])
{
    int argc = 0;
    if (!path) {
        return -1;
    }
    if (argv) {
        while (argv[argc]) {
            argc++;
        }
    }
    return (int)tnu_syscall(SYS_EXEC, (long)path, argc, (long)argv, 0, 0, 0);
}

int execvp(const char *file, char *const argv[])
{
    if (!file || !*file) {
        return -1;
    }
    if (file[0] == '/' || file[0] == '.') {
        return execv(file, argv);
    }
    char path[128];
    const char *prefixes[] = { "/bin/", "/usr/bin/", "/usr/games/" };
    for (size_t i = 0; i < sizeof(prefixes) / sizeof(prefixes[0]); i++) {
        size_t need = strlen(prefixes[i]) + strlen(file) + 1;
        if (need > sizeof(path)) {
            continue;
        }
        strcpy(path, prefixes[i]);
        strcat(path, file);
        if (access(path, X_OK) == 0) {
            return execv(path, argv);
        }
    }
    return execv(file, argv);
}

int wait(int pid)
{
    return (int)tnu_syscall(SYS_WAIT, pid, 0, 0, 0, 0, 0);
}

void exit(int code)
{
    run_atexit_handlers();
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

int fstat(int fd, struct stat *st)
{
    return (int)tnu_syscall(SYS_FSTAT, fd, (long)st, 0, 0, 0, 0);
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

int readdir_fd(int fd, struct syscall_dirent *out)
{
    return (int)tnu_syscall(SYS_READDIR, fd, (long)out, 0, 0, 0, 0);
}

uint64_t uptime_ms(void)
{
    return (uint64_t)tnu_syscall(SYS_UPTIME_MS, 0, 0, 0, 0, 0, 0);
}

int brk(void *addr)
{
    long ret = tnu_syscall(SYS_BRK, (long)addr, 0, 0, 0, 0, 0);
    return ret == (long)addr ? 0 : -1;
}

void *sbrk(intptr_t increment)
{
    uintptr_t current = (uintptr_t)tnu_syscall(SYS_BRK, 0, 0, 0, 0, 0, 0);
    uintptr_t next = current + (uintptr_t)increment;
    if (increment < 0 && next > current) {
        return (void *)-1;
    }
    if (tnu_syscall(SYS_BRK, (long)next, 0, 0, 0, 0, 0) != (long)next) {
        return (void *)-1;
    }
    return (void *)current;
}

/* Block device I/O functions for disk installation */
long block_get_count(void)
{
    return tnu_syscall(SYS_BLOCK_GET_COUNT, 0, 0, 0, 0, 0, 0);
}

int block_get_info(long index, struct block_info *info)
{
    if (!info) {
        return -1;
    }
    return (int)tnu_syscall(SYS_BLOCK_GET_INFO, index, (long)info, 0, 0, 0, 0);
}

ssize_t block_read(long index, uint64_t lba, void *buf, size_t bytes)
{
    return tnu_syscall(SYS_BLOCK_READ, index, (long)lba, (long)buf, (long)bytes, 0, 0);
}

ssize_t block_write(long index, uint64_t lba, const void *buf, size_t bytes)
{
    return tnu_syscall(SYS_BLOCK_WRITE, index, (long)lba, (long)buf, (long)bytes, 0, 0);
}

int block_sync(long index)
{
    return (int)tnu_syscall(SYS_BLOCK_SYNC, index, 0, 0, 0, 0, 0);
}

int login_session(const char *user, const char *password)
{
    return (int)tnu_syscall(SYS_LOGIN, (long)user, (long)password, 0, 0, 0, 0);
}

/* sync() — flush the persistent TFS image to disk immediately.
 * Returns 0 on success or if filesystem is not persistent,
 * -1 if a write error occurred. */
int sync(void)
{
    return (int)tnu_syscall(SYS_SYNC, 0, 0, 0, 0, 0, 0);
}

int fsync(int fd)
{
    (void)fd;
    return sync();
}

int fdatasync(int fd)
{
    return fsync(fd);
}

int shutdown(void)
{
    return (int)tnu_syscall(SYS_SHUTDOWN, 0, 0, 0, 0, 0, 0);
}

int reboot(void)
{
    return (int)tnu_syscall(SYS_REBOOT, 0, 0, 0, 0, 0, 0);
}
int wifi_scan(struct wifi_ap *out, size_t max_aps)
{
    return (int)tnu_syscall(SYS_WIFI_SCAN, (long)out, (long)max_aps, 0, 0, 0, 0);
}

int wifi_connect(const char *iface, const char *ssid, const char *passphrase)
{
    return (int)tnu_syscall(SYS_WIFI_CONNECT, (long)iface, (long)ssid,
                            (long)passphrase, 0, 0, 0);
}

int wifi_disconnect(const char *iface)
{
    return (int)tnu_syscall(SYS_WIFI_DISCONNECT, (long)iface, 0, 0, 0, 0, 0);
}

int wifi_status(struct wifi_status *out)
{
    return (int)tnu_syscall(SYS_WIFI_STATUS, (long)out, 0, 0, 0, 0, 0);
}

int resolve4(const char *host, uint32_t *out_ipv4)
{
    return (int)tnu_syscall(SYS_RESOLVE4, (long)host, (long)out_ipv4, 0, 0, 0, 0);
}
