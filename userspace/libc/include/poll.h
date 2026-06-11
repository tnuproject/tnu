#ifndef TNU_POLL_H
#define TNU_POLL_H

typedef unsigned long nfds_t;

struct pollfd {
    int fd;
    short events;
    short revents;
};

#define POLLIN  0x0001
#define POLLOUT 0x0004
#define POLLERR 0x0008

int poll(struct pollfd *fds, nfds_t nfds, int timeout);

#endif
