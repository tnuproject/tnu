/*
 * select.c — POSIX select/pselect wrappers for TNU
 */

#include <sys/select.h>
#include <sys/time.h>
#include <errno.h>
#include <tnu/syscall.h>

extern long tnu_syscall(long number, long a0, long a1, long a2, long a3, long a4, long a5);

int select(int nfds, fd_set *readfds, fd_set *writefds, fd_set *exceptfds, struct timeval *timeout)
{
    struct timespec ts;
    struct timespec *tsp = NULL;

    if (timeout) {
        ts.tv_sec = timeout->tv_sec;
        ts.tv_nsec = timeout->tv_usec * 1000;
        tsp = &ts;
    }

    long ret = tnu_syscall(SYS_SELECT, nfds, (long)readfds, (long)writefds, 
                          (long)exceptfds, (long)tsp, 0);
    
    if (ret < 0) {
        errno = EINVAL;
        return -1;
    }
    
    return (int)ret;
}

int pselect(int nfds, fd_set *readfds, fd_set *writefds, fd_set *exceptfds,
            const struct timespec *timeout, const sigset_t *sigmask)
{
    /* TNU pselect6 ABI: 6th arg is pointer to { sigset_t*; size_t } */
    struct {
        const sigset_t *ss;
        size_t ss_len;
    } sigmask_arg;
    
    sigmask_arg.ss = sigmask;
    sigmask_arg.ss_len = sigmask ? sizeof(sigset_t) : 0;
    
    long ret = tnu_syscall(SYS_PSELECT, nfds, (long)readfds, (long)writefds,
                          (long)exceptfds, (long)timeout, (long)&sigmask_arg);
    
    if (ret < 0) {
        errno = EINVAL;
        return -1;
    }
    
    return (int)ret;
}
