/*
 * Linux IPC syscalls for TNU (pipe, pipe2, epoll)
 *
 * Implements pipe(), pipe2(), epoll_create(), epoll_create1(), epoll_ctl(),
 * epoll_wait() for Linux binary compatibility.
 */

#include <tnu/linux_compat.h>
#include <tnu/log.h>
#include <tnu/memory.h>
#include <tnu/process.h>
#include <tnu/string.h>

#include "../syscall/linux_errno.h"

/*
 * pipe() - create pipe (read fd, write fd)
 * For now, returns -ENOSYS. Real implementation would allocate two FDs backed
 * by a circular buffer in kernel memory.
 */
long linux_pipe(int *pipefd)
{
    (void)pipefd;
    return -LINUX_ENOSYS;
}

/*
 * pipe2() - create pipe with flags (O_NONBLOCK, O_CLOEXEC)
 */
long linux_pipe2(int *pipefd, int flags)
{
    (void)pipefd;
    (void)flags;
    return -LINUX_ENOSYS;
}

/*
 * epoll_create() - create an epoll file descriptor
 */
long linux_epoll_create(int size)
{
    (void)size;
    return -LINUX_ENOSYS;
}

/*
 * epoll_create1() - create an epoll file descriptor with flags
 */
long linux_epoll_create1(int flags)
{
    (void)flags;
    return -LINUX_ENOSYS;
}

/*
 * epoll_ctl() - control interface for an epoll descriptor
 */
long linux_epoll_ctl(int epfd, int op, int fd, void *event)
{
    (void)epfd;
    (void)op;
    (void)fd;
    (void)event;
    return -LINUX_ENOSYS;
}

/*
 * epoll_wait() - wait for an I/O event on an epoll file descriptor
 */
long linux_epoll_wait(int epfd, void *events, int maxevents, int timeout)
{
    (void)epfd;
    (void)events;
    (void)maxevents;
    (void)timeout;
    return -LINUX_ENOSYS;
}

void linux_ipc_init(void)
{
    log_info("linux-compat", "Linux IPC layer initialized (stubs only)");
}
