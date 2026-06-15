#ifndef TNU_SYS_SELECT_H
#define TNU_SYS_SELECT_H

#include <sys/types.h>
#include <sys/time.h>
#include <signal.h>
#include <time.h>

typedef struct {
    unsigned long fds_bits[16];
} fd_set;

#define FD_SETSIZE 1024

void FD_ZERO(fd_set *set);
void FD_SET(int fd, fd_set *set);
void FD_CLR(int fd, fd_set *set);
int FD_ISSET(int fd, fd_set *set);

int select(int nfds, fd_set *readfds, fd_set *writefds, fd_set *exceptfds,
           struct timeval *timeout);
int pselect(int nfds, fd_set *readfds, fd_set *writefds, fd_set *exceptfds,
            const struct timespec *timeout, const sigset_t *sigmask);

#endif
