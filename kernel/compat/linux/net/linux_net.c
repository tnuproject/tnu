/*
 * Linux networking syscall bridge for TNU
 *
 * This file implements Linux socket syscalls (socket, bind, connect, sendto,
 * recvfrom, listen, accept, etc.) by bridging to the TNU native networking
 * stack when available. For applications that don't require actual networking,
 * it returns -ENOSYS or stubs as appropriate.
 *
 * iwlwifi driver integration: When a WiFi device is available via /dev/wlan0
 * or similar, this layer will use the TNU net interface to provide socket
 * functionality.
 */

#include <tnu/linux_compat.h>
#include <tnu/log.h>
#include <tnu/memory.h>
#include <tnu/net.h>
#include <tnu/process.h>
#include <tnu/string.h>
#include <tnu/vfs.h>

#include "../syscall/linux_errno.h"

/* Linux socket address families */
#define LINUX_AF_UNSPEC  0
#define LINUX_AF_INET    2
#define LINUX_AF_INET6  10

/* Linux socket types */
#define LINUX_SOCK_STREAM    1
#define LINUX_SOCK_DGRAM     2
#define LINUX_SOCK_RAW       3
#define LINUX_SOCK_NONBLOCK  0x800
#define LINUX_SOCK_CLOEXEC   0x80000

/* Linux socket options */
#define LINUX_SOL_SOCKET     1
#define LINUX_SO_REUSEADDR   2
#define LINUX_SO_KEEPALIVE   9
#define LINUX_SO_RCVTIMEO   20
#define LINUX_SO_SNDTIMEO   21

/* Linux socket flags */
#define LINUX_MSG_DONTWAIT   0x40

/* Placeholder socket descriptor structure */
struct linux_socket {
    int type;  /* SOCK_STREAM, SOCK_DGRAM, SOCK_RAW */
    int domain;
    bool connected;
    uint32_t local_ip;
    uint16_t local_port;
    uint32_t remote_ip;
    uint16_t remote_port;
};

/*
 * socket() - create an endpoint for communication
 *
 * For now, we return -ENOSYS for all socket calls. When the TNU net stack
 * is ready with socket support, this will allocate a file descriptor backed
 * by a linux_socket structure.
 */
long linux_socket(int domain, int type, int protocol)
{
    (void)domain;
    (void)type;
    (void)protocol;

    /* TODO: When TNU net stack has socket support, allocate FD and
     * linux_socket here. For now, return ENOSYS to indicate networking
     * is not yet fully implemented. */
    return -LINUX_ENOSYS;
}

long linux_bind(int sockfd, const void *addr, uint32_t addrlen)
{
    (void)sockfd;
    (void)addr;
    (void)addrlen;
    return -LINUX_ENOSYS;
}

long linux_connect(int sockfd, const void *addr, uint32_t addrlen)
{
    (void)sockfd;
    (void)addr;
    (void)addrlen;
    return -LINUX_ENOSYS;
}

long linux_listen(int sockfd, int backlog)
{
    (void)sockfd;
    (void)backlog;
    return -LINUX_ENOSYS;
}

long linux_accept(int sockfd, void *addr, uint32_t *addrlen)
{
    (void)sockfd;
    (void)addr;
    (void)addrlen;
    return -LINUX_ENOSYS;
}

long linux_accept4(int sockfd, void *addr, uint32_t *addrlen, int flags)
{
    (void)sockfd;
    (void)addr;
    (void)addrlen;
    (void)flags;
    return -LINUX_ENOSYS;
}

long linux_sendto(int sockfd, const void *buf, size_t len, int flags,
                  const void *dest_addr, uint32_t addrlen)
{
    (void)sockfd;
    (void)buf;
    (void)len;
    (void)flags;
    (void)dest_addr;
    (void)addrlen;
    return -LINUX_ENOSYS;
}

long linux_recvfrom(int sockfd, void *buf, size_t len, int flags,
                    void *src_addr, uint32_t *addrlen)
{
    (void)sockfd;
    (void)buf;
    (void)len;
    (void)flags;
    (void)src_addr;
    (void)addrlen;
    return -LINUX_ENOSYS;
}

long linux_sendmsg(int sockfd, const void *msg, int flags)
{
    (void)sockfd;
    (void)msg;
    (void)flags;
    return -LINUX_ENOSYS;
}

long linux_recvmsg(int sockfd, void *msg, int flags)
{
    (void)sockfd;
    (void)msg;
    (void)flags;
    return -LINUX_ENOSYS;
}

long linux_getsockname(int sockfd, void *addr, uint32_t *addrlen)
{
    (void)sockfd;
    (void)addr;
    (void)addrlen;
    return -LINUX_ENOSYS;
}

long linux_getpeername(int sockfd, void *addr, uint32_t *addrlen)
{
    (void)sockfd;
    (void)addr;
    (void)addrlen;
    return -LINUX_ENOSYS;
}

long linux_setsockopt(int sockfd, int level, int optname, const void *optval,
                      uint32_t optlen)
{
    (void)sockfd;
    (void)level;
    (void)optname;
    (void)optval;
    (void)optlen;
    /* Allow setsockopt to "succeed" without doing anything for now */
    return 0;
}

long linux_getsockopt(int sockfd, int level, int optname, void *optval,
                      uint32_t *optlen)
{
    (void)sockfd;
    (void)level;
    (void)optname;
    (void)optval;
    (void)optlen;
    return -LINUX_ENOSYS;
}

void linux_net_init(void)
{
    log_info("linux-compat", "Linux networking layer initialized (stubs only)");
}
