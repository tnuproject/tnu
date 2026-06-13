#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <locale.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <tnu/syscall.h>
#include <termios.h>
#include <unistd.h>

int raise(int sig)
{
    (void)sig;
    errno = ENOSYS;
    return -1;
}

int system(const char *command)
{
    (void)command;
    errno = ENOSYS;
    return -1;
}

void (*signal(int sig, void (*handler)(int)))(int)
{
    struct sigaction act;
    struct sigaction old;
    memset(&act, 0, sizeof(act));
    act.sa_handler = handler;
    if (sigaction(sig, &act, &old) < 0) {
        return SIG_ERR;
    }
    return old.sa_handler;
}

int sigemptyset(sigset_t *set)
{
    if (!set) {
        errno = EINVAL;
        return -1;
    }
    *set = 0;
    return 0;
}

int sigaction(int sig, const struct sigaction *act, struct sigaction *oldact)
{
    static struct sigaction actions[32];
    if (sig < 0 || sig >= (int)(sizeof(actions) / sizeof(actions[0]))) {
        errno = EINVAL;
        return -1;
    }
    if (tnu_syscall(SYS_SIGACTION, sig, (long)act, (long)oldact, 0, 0, 0) < 0) {
        errno = EINVAL;
        return -1;
    }
    if (oldact) {
        *oldact = actions[sig];
    }
    if (act) {
        actions[sig] = *act;
    }
    return 0;
}

int tcgetattr(int fd, struct termios *termios_p)
{
    (void)fd;
    if (!termios_p) {
        errno = EINVAL;
        return -1;
    }
    memset(termios_p, 0, sizeof(*termios_p));
    termios_p->c_lflag = ECHO | ICANON | ISIG | IEXTEN;
    termios_p->c_iflag = IXON;
    termios_p->c_oflag = OPOST;
    return 0;
}

int tcsetattr(int fd, int optional_actions, const struct termios *termios_p)
{
    (void)fd;
    (void)optional_actions;
    (void)termios_p;
    return 0;
}

struct DIR {
    int fd;
    struct dirent entry;
};

DIR *opendir(const char *name)
{
    int fd = open(name, O_RDONLY);
    if (fd < 0) {
        errno = ENOENT;
        return 0;
    }
    DIR *dir = malloc(sizeof(*dir));
    if (!dir) {
        close(fd);
        errno = ENOMEM;
        return 0;
    }
    dir->fd = fd;
    return dir;
}

struct dirent *readdir(DIR *dirp)
{
    if (!dirp) {
        errno = EBADF;
        return 0;
    }
    struct syscall_dirent kdent;
    int rc = readdir_fd(dirp->fd, &kdent);
    if (rc <= 0) {
        if (rc < 0) {
            errno = ENOTDIR;
        }
        return 0;
    }
    dirp->entry.d_ino = kdent.d_ino;
    dirp->entry.d_type = kdent.d_type;
    strncpy(dirp->entry.d_name, kdent.d_name, sizeof(dirp->entry.d_name) - 1);
    dirp->entry.d_name[sizeof(dirp->entry.d_name) - 1] = '\0';
    return &dirp->entry;
}

int closedir(DIR *dirp)
{
    if (!dirp) {
        errno = EBADF;
        return -1;
    }
    int rc = close(dirp->fd);
    free(dirp);
    return rc;
}

char *setlocale(int category, const char *locale)
{
    (void)category;
    return locale && locale[0] && strcmp(locale, "C") != 0 ? 0 : "C";
}

uint16_t htons(uint16_t hostshort)
{
    return (uint16_t)((hostshort << 8) | (hostshort >> 8));
}

uint16_t ntohs(uint16_t netshort)
{
    return htons(netshort);
}

uint32_t htonl(uint32_t hostlong)
{
    return ((hostlong & 0x000000ffu) << 24) |
           ((hostlong & 0x0000ff00u) << 8) |
           ((hostlong & 0x00ff0000u) >> 8) |
           ((hostlong & 0xff000000u) >> 24);
}

uint32_t ntohl(uint32_t netlong)
{
    return htonl(netlong);
}

int inet_pton(int af, const char *src, void *dst)
{
    if (af != AF_INET || !src || !dst) {
        errno = EINVAL;
        return -1;
    }
    uint32_t octets[4] = {0, 0, 0, 0};
    for (int i = 0; i < 4; i++) {
        if (!*src) {
            return 0;
        }
        while (*src >= '0' && *src <= '9') {
            octets[i] = octets[i] * 10 + (uint32_t)(*src - '0');
            if (octets[i] > 255) {
                return 0;
            }
            src++;
        }
        if (i < 3) {
            if (*src++ != '.') {
                return 0;
            }
        }
    }
    if (*src) {
        return 0;
    }
    *(uint32_t *)dst = htonl((octets[0] << 24) | (octets[1] << 16) |
                             (octets[2] << 8) | octets[3]);
    return 1;
}

const char *inet_ntop(int af, const void *src, char *dst, socklen_t size)
{
    if (af != AF_INET || !src || !dst || size < 16) {
        errno = EINVAL;
        return 0;
    }
    uint32_t ip = ntohl(*(const uint32_t *)src);
    int written = 0;
    unsigned octets[4] = {
        (ip >> 24) & 0xff,
        (ip >> 16) & 0xff,
        (ip >> 8) & 0xff,
        ip & 0xff,
    };
    char *p = dst;
    for (int i = 0; i < 4; i++) {
        char tmp[4];
        int n = 0;
        unsigned v = octets[i];
        if (v >= 100) tmp[n++] = (char)('0' + v / 100);
        if (v >= 10) tmp[n++] = (char)('0' + (v / 10) % 10);
        tmp[n++] = (char)('0' + v % 10);
        for (int j = 0; j < n; j++) {
            *p++ = tmp[j];
            written++;
        }
        if (i != 3) {
            *p++ = '.';
            written++;
        }
    }
    *p = '\0';
    return written < (int)size ? dst : 0;
}

int socket(int domain, int type, int protocol)
{
    (void)domain;
    (void)type;
    (void)protocol;
    errno = ENOSYS;
    return -1;
}

int connect(int sockfd, const struct sockaddr *addr, socklen_t addrlen)
{
    (void)sockfd;
    (void)addr;
    (void)addrlen;
    errno = ENOSYS;
    return -1;
}

ssize_t send(int sockfd, const void *buf, size_t len, int flags)
{
    (void)sockfd;
    (void)buf;
    (void)len;
    (void)flags;
    errno = ENOSYS;
    return -1;
}

ssize_t recv(int sockfd, void *buf, size_t len, int flags)
{
    (void)sockfd;
    (void)buf;
    (void)len;
    (void)flags;
    errno = ENOSYS;
    return -1;
}

int bind(int sockfd, const struct sockaddr *addr, socklen_t addrlen)
{
    (void)sockfd;
    (void)addr;
    (void)addrlen;
    errno = ENOSYS;
    return -1;
}

int listen(int sockfd, int backlog)
{
    (void)sockfd;
    (void)backlog;
    errno = ENOSYS;
    return -1;
}

int accept(int sockfd, struct sockaddr *addr, socklen_t *addrlen)
{
    (void)sockfd;
    (void)addr;
    (void)addrlen;
    errno = ENOSYS;
    return -1;
}

void FD_ZERO(fd_set *set)
{
    memset(set, 0, sizeof(*set));
}

void FD_SET(int fd, fd_set *set)
{
    if (fd >= 0 && fd < FD_SETSIZE) {
        set->fds_bits[fd / (8 * (int)sizeof(unsigned long))] |=
            1ul << (fd % (8 * (int)sizeof(unsigned long)));
    }
}

void FD_CLR(int fd, fd_set *set)
{
    if (fd >= 0 && fd < FD_SETSIZE) {
        set->fds_bits[fd / (8 * (int)sizeof(unsigned long))] &=
            ~(1ul << (fd % (8 * (int)sizeof(unsigned long))));
    }
}

int FD_ISSET(int fd, fd_set *set)
{
    if (fd < 0 || fd >= FD_SETSIZE) {
        return 0;
    }
    return (set->fds_bits[fd / (8 * (int)sizeof(unsigned long))] &
            (1ul << (fd % (8 * (int)sizeof(unsigned long))))) != 0;
}

int select(int nfds, fd_set *readfds, fd_set *writefds, fd_set *exceptfds,
           struct timeval *timeout)
{
    (void)nfds;
    (void)readfds;
    (void)writefds;
    (void)exceptfds;
    (void)timeout;
    errno = ENOSYS;
    return -1;
}

int poll(struct pollfd *fds, nfds_t nfds, int timeout)
{
    (void)fds;
    (void)nfds;
    (void)timeout;
    errno = ENOSYS;
    return -1;
}

void *mmap(void *addr, size_t length, int prot, int flags, int fd, long offset)
{
    (void)addr;
    (void)prot;
    if (length == 0 || (flags & MAP_SHARED)) {
        errno = EINVAL;
        return MAP_FAILED;
    }
    void *mem = malloc(length);
    if (!mem) {
        errno = ENOMEM;
        return MAP_FAILED;
    }
    if ((flags & MAP_ANONYMOUS) || fd < 0) {
        memset(mem, 0, length);
        return mem;
    }

    off_t old = lseek(fd, 0, SEEK_CUR);
    if (offset >= 0 && lseek(fd, offset, SEEK_SET) < 0) {
        free(mem);
        return MAP_FAILED;
    }
    ssize_t got = read(fd, mem, length);
    if (got < 0) {
        if (old >= 0) {
            lseek(fd, old, SEEK_SET);
        }
        free(mem);
        return MAP_FAILED;
    }
    if ((size_t)got < length) {
        memset((char *)mem + got, 0, length - (size_t)got);
    }
    if (old >= 0) {
        lseek(fd, old, SEEK_SET);
    }
    return mem;
}

int munmap(void *addr, size_t length)
{
    (void)length;
    free(addr);
    return 0;
}

int ioctl(int fd, unsigned long request, ...)
{
    va_list ap;
    va_start(ap, request);
    void *arg = va_arg(ap, void *);
    va_end(ap);
    int rc = (int)tnu_syscall(SYS_IOCTL, fd, (long)request, (long)arg, 0, 0, 0);
    if (rc < 0) {
        errno = ENOSYS;
    }
    return rc;
}

int isatty(int fd)
{
    return fd >= 0 && fd <= 2;
}
