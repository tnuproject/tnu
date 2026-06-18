#ifndef TNU_LINUX_COMPAT_H
#define TNU_LINUX_COMPAT_H

#include <tnu/types.h>

#define LINUX_PERSONALITY 0x4c4e5801u

#define LINUX_AT_NULL    0
#define LINUX_AT_PHDR    3
#define LINUX_AT_PHENT   4
#define LINUX_AT_PHNUM   5
#define LINUX_AT_PAGESZ  6
#define LINUX_AT_BASE    7
#define LINUX_AT_ENTRY   9
#define LINUX_AT_UID    11
#define LINUX_AT_EUID   12
#define LINUX_AT_GID    13
#define LINUX_AT_EGID   14
#define LINUX_AT_HWCAP  16
#define LINUX_AT_CLKTCK 17
#define LINUX_AT_SECURE 23
#define LINUX_AT_RANDOM 25
#define LINUX_AT_EXECFN 31

enum linux_elf_class {
    LINUX_ELF_NONE = 0,
    LINUX_ELF32 = 1,
    LINUX_ELF64 = 2,
};

struct linux_elf_info {
    enum linux_elf_class elf_class;
    uint16_t machine;
    uint16_t type;
    uint64_t entry;
    uint64_t phoff;
    uint16_t phentsize;
    uint16_t phnum;
    uint64_t lowest_vaddr;
    uint64_t highest_vaddr;
    uint64_t interp_offset;
    uint64_t interp_size;
    uint64_t tls_vaddr;
    uint64_t tls_memsz;
    uint64_t tls_filesz;
    uint64_t relro_vaddr;
    uint64_t relro_memsz;
    uint64_t stack_flags;
    bool has_interp;
    bool has_tls;
    bool has_relro;
    bool has_gnu_stack;
    bool is_pie;
};

struct linux_syscall_args {
    uint64_t nr;
    uint64_t a0;
    uint64_t a1;
    uint64_t a2;
    uint64_t a3;
    uint64_t a4;
    uint64_t a5;
};

void linux_compat_init(void);
int linux_elf_probe(const void *image, size_t size, struct linux_elf_info *info);
long linux_run_binary(const char *path, int argc, char **argv);
void linux_mm_reset(uintptr_t brk_floor, uintptr_t mmap_base, uintptr_t mmap_limit);
long linux_execve(const char *path, char *const argv[], char *const envp[]);
long linux_execveat(int dirfd, const char *path, char *const argv[],
                    char *const envp[], int flags);
long linux_syscall_entry(uint64_t nr, uint64_t a0, uint64_t a1, uint64_t a2,
                         uint64_t a3, uint64_t a4, uint64_t a5);
long linux_syscall_dispatch(const struct linux_syscall_args *args);
const char *linux_syscall_name(uint64_t nr);

/* Linux networking (socket syscalls) */
void linux_net_init(void);
long linux_socket(int domain, int type, int protocol);
long linux_bind(int sockfd, const void *addr, uint32_t addrlen);
long linux_connect(int sockfd, const void *addr, uint32_t addrlen);
long linux_listen(int sockfd, int backlog);
long linux_accept(int sockfd, void *addr, uint32_t *addrlen);
long linux_accept4(int sockfd, void *addr, uint32_t *addrlen, int flags);
long linux_sendto(int sockfd, const void *buf, size_t len, int flags,
                  const void *dest_addr, uint32_t addrlen);
long linux_recvfrom(int sockfd, void *buf, size_t len, int flags,
                    void *src_addr, uint32_t *addrlen);
long linux_sendmsg(int sockfd, const void *msg, int flags);
long linux_recvmsg(int sockfd, void *msg, int flags);
long linux_getsockname(int sockfd, void *addr, uint32_t *addrlen);
long linux_getpeername(int sockfd, void *addr, uint32_t *addrlen);
long linux_setsockopt(int sockfd, int level, int optname, const void *optval,
                      uint32_t optlen);
long linux_getsockopt(int sockfd, int level, int optname, void *optval,
                      uint32_t *optlen);

/* Linux IPC (pipe, epoll) */
void linux_ipc_init(void);
long linux_pipe(int *pipefd);
long linux_pipe2(int *pipefd, int flags);
long linux_epoll_create(int size);
long linux_epoll_create1(int flags);
long linux_epoll_ctl(int epfd, int op, int fd, void *event);
long linux_epoll_wait(int epfd, void *events, int maxevents, int timeout);

/* Linux signals */
void linux_signal_init(void);
long linux_rt_sigaction(int signum, const void *act, void *oldact, size_t sigsetsize);
long linux_rt_sigprocmask(int how, const void *set, void *oldset, size_t sigsetsize);
long linux_rt_sigreturn(void);
long linux_sigaltstack(const void *ss, void *old_ss);

/* Linux process (fork/clone) */
void linux_proc_init(void);
long linux_fork(void);
long linux_vfork(void);
long linux_clone(unsigned long flags, void *stack, int *parent_tid,
                 int *child_tid, unsigned long tls);

#endif
