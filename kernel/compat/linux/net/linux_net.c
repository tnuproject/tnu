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

struct linux_sockaddr_in {
    uint16_t family;
    uint16_t port;
    uint32_t addr;
    uint8_t zero[8];
} __attribute__((packed));

struct linux_net_iovec {
    void *base;
    size_t len;
};

struct linux_net_msghdr {
    void *name;
    uint32_t namelen;
    struct linux_net_iovec *iov;
    size_t iovlen;
    void *control;
    size_t controllen;
    int flags;
};

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

static uint16_t linux_be16(const void *p)
{
    const uint8_t *b = p;
    return ((uint16_t)b[0] << 8) | b[1];
}

static uint32_t linux_be32(const void *p)
{
    const uint8_t *b = p;
    return ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) |
           ((uint32_t)b[2] << 8) | b[3];
}

/*
 * socket() - create an endpoint for communication
 *
 * For now, we return -ENOSYS for all socket calls. When the TNU net stack
 * is ready with socket support, this will allocate a file descriptor backed
 * by a linux_socket structure.
 */
long linux_socket(int domain, int type, int protocol)
{
    int base_type = type & ~(LINUX_SOCK_NONBLOCK | LINUX_SOCK_CLOEXEC);
    if (domain != LINUX_AF_INET || base_type != LINUX_SOCK_STREAM) {
        return -LINUX_EAFNOSUPPORT;
    }
    int fd = net_socket_create(2, 1, protocol);
    return fd < 0 ? -LINUX_ENOSYS : fd;
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
    if (!addr || addrlen < sizeof(struct linux_sockaddr_in)) {
        return -LINUX_EINVAL;
    }
    const struct linux_sockaddr_in *in = addr;
    if (in->family != LINUX_AF_INET) {
        return -LINUX_EAFNOSUPPORT;
    }
    uint32_t ip = linux_be32(&in->addr);
    uint16_t port = linux_be16(&in->port);
    return net_socket_connect(sockfd, ip, port) < 0 ? -LINUX_ECONNREFUSED : 0;
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
    (void)flags;
    if (dest_addr && addrlen >= sizeof(struct linux_sockaddr_in)) {
        long rc = linux_connect(sockfd, dest_addr, addrlen);
        if (rc < 0) {
            return rc;
        }
    }
    ssize_t sent = net_socket_send(sockfd, buf, len);
    return sent < 0 ? -LINUX_EIO : sent;
}

long linux_recvfrom(int sockfd, void *buf, size_t len, int flags,
                    void *src_addr, uint32_t *addrlen)
{
    (void)flags;
    (void)src_addr;
    if (addrlen) {
        *addrlen = 0;
    }
    ssize_t got = net_socket_recv(sockfd, buf, len);
    return got < 0 ? -LINUX_EIO : got;
}

long linux_sendmsg(int sockfd, const void *msg, int flags)
{
    const struct linux_net_msghdr *hdr = msg;
    if (!hdr || (!hdr->iov && hdr->iovlen)) {
        return -LINUX_EINVAL;
    }
    if (hdr->name && hdr->namelen >= sizeof(struct linux_sockaddr_in)) {
        long rc = linux_connect(sockfd, hdr->name, hdr->namelen);
        if (rc < 0) {
            return rc;
        }
    }
    size_t total = 0;
    for (size_t i = 0; i < hdr->iovlen; i++) {
        if (!hdr->iov[i].base && hdr->iov[i].len) {
            return total ? (long)total : -LINUX_EINVAL;
        }
        if (hdr->iov[i].len == 0) {
            continue;
        }
        long sent = linux_sendto(sockfd, hdr->iov[i].base, hdr->iov[i].len,
                                 flags, NULL, 0);
        if (sent < 0) {
            return total ? (long)total : sent;
        }
        total += (size_t)sent;
        if ((size_t)sent < hdr->iov[i].len) {
            break;
        }
    }
    return (long)total;
}

long linux_recvmsg(int sockfd, void *msg, int flags)
{
    struct linux_net_msghdr *hdr = msg;
    if (!hdr || (!hdr->iov && hdr->iovlen)) {
        return -LINUX_EINVAL;
    }
    size_t total = 0;
    for (size_t i = 0; i < hdr->iovlen; i++) {
        if (!hdr->iov[i].base && hdr->iov[i].len) {
            return total ? (long)total : -LINUX_EINVAL;
        }
        if (hdr->iov[i].len == 0) {
            continue;
        }
        long got = linux_recvfrom(sockfd, hdr->iov[i].base, hdr->iov[i].len,
                                  flags, NULL, NULL);
        if (got < 0) {
            return total ? (long)total : got;
        }
        total += (size_t)got;
        if ((size_t)got < hdr->iov[i].len) {
            break;
        }
    }
    hdr->flags = 0;
    return (long)total;
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
    log_info("linux-compat", "Linux networking layer initialized");
}
