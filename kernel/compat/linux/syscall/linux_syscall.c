#include <arch/cpu.h>

#include <tnu/linux_compat.h>
#include <tnu/memory.h>
#include <tnu/process.h>
#include <tnu/printf.h>
#include <tnu/scheduler.h>
#include <tnu/string.h>
#include <tnu/syscall.h>
#include <tnu/vfs.h>

#include "linux_errno.h"
#include "linux_syscall_table.h"

#define AT_FDCWD -100
#define AT_EMPTY_PATH 0x1000
#define LINUX_UTS_LEN 65
#define LINUX_MAP_SHARED  0x01
#define LINUX_MAP_PRIVATE 0x02
#define LINUX_MAP_FIXED   0x10
#define LINUX_MAP_ANON    0x20
#define LINUX_ARCH_SET_FS 0x1002
#define LINUX_ARCH_GET_FS 0x1003
#define LINUX_FUTEX_WAIT 0
#define LINUX_FUTEX_WAKE 1
#define LINUX_O_ACCMODE 00000003
#define LINUX_O_RDONLY  00000000
#define LINUX_O_WRONLY  00000001
#define LINUX_O_RDWR    00000002
#define LINUX_O_CREAT   00000100
#define LINUX_O_EXCL    00000200
#define LINUX_O_TRUNC   00001000
#define LINUX_O_APPEND  00002000
#define LINUX_O_NONBLOCK 00004000
#define LINUX_DT_UNKNOWN 0
#define LINUX_DT_CHR 2
#define LINUX_DT_DIR 4
#define LINUX_DT_REG 8

static uintptr_t linux_brk_floor;
static uintptr_t linux_brk_current;
static uintptr_t linux_brk_limit;
static uintptr_t linux_mmap_next;
static uintptr_t linux_mmap_limit;

struct linux_timespec {
    int64_t tv_sec;
    int64_t tv_nsec;
};

struct linux_timeval {
    int64_t tv_sec;
    int64_t tv_usec;
};

struct linux_utsname {
    char sysname[LINUX_UTS_LEN];
    char nodename[LINUX_UTS_LEN];
    char release[LINUX_UTS_LEN];
    char version[LINUX_UTS_LEN];
    char machine[LINUX_UTS_LEN];
    char domainname[LINUX_UTS_LEN];
};

struct linux_stat {
    uint64_t st_dev;
    uint64_t st_ino;
    uint64_t st_nlink;
    uint32_t st_mode;
    uint32_t st_uid;
    uint32_t st_gid;
    uint32_t __pad0;
    uint64_t st_rdev;
    int64_t st_size;
    int64_t st_blksize;
    int64_t st_blocks;
    int64_t st_atime;
    int64_t st_atime_nsec;
    int64_t st_mtime;
    int64_t st_mtime_nsec;
    int64_t st_ctime;
    int64_t st_ctime_nsec;
    int64_t __unused[3];
};

struct linux_iovec {
    void *iov_base;
    uint64_t iov_len;
};

struct linux_rlimit64 {
    uint64_t rlim_cur;
    uint64_t rlim_max;
};

struct linux_sysinfo {
    int64_t uptime;
    uint64_t loads[3];
    uint64_t totalram;
    uint64_t freeram;
    uint64_t sharedram;
    uint64_t bufferram;
    uint64_t totalswap;
    uint64_t freeswap;
    uint16_t procs;
    uint16_t pad;
    uint64_t totalhigh;
    uint64_t freehigh;
    uint32_t mem_unit;
    char _f[0];
};

struct linux_dirent_ctx {
    char *buf;
    size_t size;
    size_t written;
    uint64_t index;
    uint64_t start;
    int emitted;
};

static uintptr_t linux_page_up(uintptr_t value)
{
    return (value + PAGE_SIZE - 1) & ~(uintptr_t)(PAGE_SIZE - 1);
}

void linux_mm_reset(uintptr_t brk_floor, uintptr_t mmap_base, uintptr_t mmap_limit)
{
    linux_brk_floor = linux_page_up(brk_floor);
    linux_brk_current = linux_brk_floor;
    linux_brk_limit = mmap_base;
    linux_mmap_next = linux_page_up(mmap_base);
    linux_mmap_limit = mmap_limit;
}

static long native_dispatch(uint64_t number, uint64_t a0, uint64_t a1, uint64_t a2,
                            uint64_t a3, uint64_t a4, uint64_t a5)
{
    struct process *proc = process_current();
    uint32_t saved = proc ? proc->personality : 0;
    if (proc) {
        proc->personality = 0;
    }
    long rc = syscall_dispatch(number, a0, a1, a2, a3, a4, a5);
    if (proc) {
        proc->personality = saved;
    }
    return rc;
}

static long native_to_linux(long rc)
{
    return rc < 0 ? -LINUX_EINVAL : rc;
}

static int linux_resolve_path(const char *path, char *out, size_t out_size)
{
    struct process *proc = process_current();
    if (!path || !out || out_size == 0) {
        return -1;
    }
    if (path[0] == '/') {
        if (strncmp(path, "/proc", 5) == 0 || strncmp(path, "/dev", 4) == 0 ||
            strncmp(path, "/sys", 4) == 0) {
            return vfs_normalize(path, "/", out, out_size);
        }
        char linux_path[VFS_PATH_MAX];
        ksnprintf(linux_path, sizeof(linux_path), "/usr/linux%s", path);
        if (vfs_lookup(linux_path, "/")) {
            strncpy(out, linux_path, out_size - 1);
            out[out_size - 1] = '\0';
            return 0;
        }
        char parent[VFS_PATH_MAX];
        strncpy(parent, linux_path, sizeof(parent) - 1);
        parent[sizeof(parent) - 1] = '\0';
        char *slash = strrchr(parent, '/');
        if (slash && slash != parent) {
            *slash = '\0';
            if (vfs_lookup(parent, "/")) {
                strncpy(out, linux_path, out_size - 1);
                out[out_size - 1] = '\0';
                return 0;
            }
        }
    }
    if (vfs_lookup(path, proc ? proc->cwd : "/")) {
        return vfs_normalize(path, proc ? proc->cwd : "/", out, out_size);
    }
    return vfs_normalize(path, proc ? proc->cwd : "/", out, out_size);
}

static int linux_open_flags_to_vfs(int flags)
{
    int out = 0;
    switch (flags & LINUX_O_ACCMODE) {
    case LINUX_O_WRONLY:
        out |= VFS_O_WRONLY;
        break;
    case LINUX_O_RDWR:
        out |= VFS_O_RDWR;
        break;
    default:
        out |= VFS_O_RDONLY;
        break;
    }
    if (flags & LINUX_O_CREAT) {
        out |= VFS_O_CREAT;
    }
    if (flags & LINUX_O_EXCL) {
        out |= VFS_O_EXCL;
    }
    if (flags & LINUX_O_TRUNC) {
        out |= VFS_O_TRUNC;
    }
    if (flags & LINUX_O_APPEND) {
        out |= VFS_O_APPEND;
    }
    if (flags & LINUX_O_NONBLOCK) {
        out |= VFS_O_NONBLOCK;
    }
    return out;
}

static long linux_open_path(const char *path, int flags, int mode)
{
    char resolved[VFS_PATH_MAX];
    if (linux_resolve_path(path, resolved, sizeof(resolved)) < 0) {
        return -LINUX_EFAULT;
    }
    long rc = native_dispatch(SYS_OPEN, (uint64_t)(uintptr_t)resolved,
                              (uint64_t)linux_open_flags_to_vfs(flags),
                              (uint64_t)mode, 0, 0, 0);
    return rc < 0 ? -LINUX_ENOENT : rc;
}

static void fill_linux_stat(const struct vfs_stat *in, struct linux_stat *out)
{
    memset(out, 0, sizeof(*out));
    out->st_dev = 1;
    out->st_ino = (uint64_t)(uintptr_t)in;
    out->st_nlink = in->type == VFS_NODE_DIR ? 2 : 1;
    out->st_mode = in->mode;
    out->st_uid = in->uid;
    out->st_gid = in->gid;
    out->st_size = (int64_t)in->size;
    out->st_blksize = 4096;
    out->st_blocks = (int64_t)((in->size + 511) / 512);
    out->st_mtime = (int64_t)in->modified;
    out->st_ctime = (int64_t)in->modified;
    out->st_atime = (int64_t)in->modified;
}

static long linux_stat_path(const char *path, struct linux_stat *out)
{
    if (!path || !out) {
        return -LINUX_EFAULT;
    }
    char resolved[VFS_PATH_MAX];
    if (linux_resolve_path(path, resolved, sizeof(resolved)) < 0) {
        return -LINUX_EFAULT;
    }
    struct process *proc = process_current();
    struct vfs_stat st;
    if (vfs_stat(resolved, proc ? proc->cwd : "/", &st) < 0) {
        return -LINUX_ENOENT;
    }
    fill_linux_stat(&st, out);
    return 0;
}

static long linux_fstat(int fd, struct linux_stat *out)
{
    struct process *proc = process_current();
    struct file_descriptor *file = process_get_fd(proc, fd);
    if (!file || !out) {
        return !out ? -LINUX_EFAULT : -LINUX_EBADF;
    }
    struct vfs_stat st;
    st.mode = file->node->mode;
    st.uid = file->node->uid;
    st.gid = file->node->gid;
    st.size = file->node->size;
    st.modified = file->node->modified;
    st.type = file->node->type;
    fill_linux_stat(&st, out);
    return 0;
}

static long linux_pread64(int fd, void *buf, size_t count, off_t offset)
{
    struct process *proc = process_current();
    struct file_descriptor *file = process_get_fd(proc, fd);
    if (!file) {
        return -LINUX_EBADF;
    }
    if (!buf && count) {
        return -LINUX_EFAULT;
    }
    ssize_t n = vfs_read_node(file->node, (uint64_t)offset, buf, count);
    return n < 0 ? -LINUX_EIO : n;
}

static long linux_pwrite64(int fd, const void *buf, size_t count, off_t offset)
{
    struct process *proc = process_current();
    struct file_descriptor *file = process_get_fd(proc, fd);
    if (!file) {
        return -LINUX_EBADF;
    }
    if (!buf && count) {
        return -LINUX_EFAULT;
    }
    ssize_t n = vfs_write_node(file->node, (uint64_t)offset, buf, count);
    return n < 0 ? -LINUX_EIO : n;
}

static long linux_native_path_syscall(uint64_t number, const char *path,
                                      uint64_t a1, uint64_t a2)
{
    char resolved[VFS_PATH_MAX];
    if (linux_resolve_path(path, resolved, sizeof(resolved)) < 0) {
        return -LINUX_EFAULT;
    }
    return native_to_linux(native_dispatch(number, (uint64_t)(uintptr_t)resolved,
                                           a1, a2, 0, 0, 0));
}

static long linux_readv(int fd, const struct linux_iovec *iov, int iovcnt)
{
    if (iovcnt == 0) {
        return 0;
    }
    if (!iov || iovcnt < 0 || iovcnt > 16) {
        return -LINUX_EINVAL;
    }
    long total = 0;
    for (int i = 0; i < iovcnt; i++) {
        long n = native_dispatch(SYS_READ, (uint64_t)fd,
                                 (uint64_t)(uintptr_t)iov[i].iov_base,
                                 iov[i].iov_len, 0, 0, 0);
        if (n < 0) {
            return total ? total : -LINUX_EIO;
        }
        total += n;
        if ((uint64_t)n < iov[i].iov_len) {
            break;
        }
    }
    return total;
}

static long linux_writev(int fd, const struct linux_iovec *iov, int iovcnt)
{
    if (iovcnt == 0) {
        return 0;
    }
    if (!iov || iovcnt < 0 || iovcnt > 16) {
        return -LINUX_EINVAL;
    }
    long total = 0;
    for (int i = 0; i < iovcnt; i++) {
        long n = native_dispatch(SYS_WRITE, (uint64_t)fd,
                                 (uint64_t)(uintptr_t)iov[i].iov_base,
                                 iov[i].iov_len, 0, 0, 0);
        if (n < 0) {
            return total ? total : -LINUX_EIO;
        }
        total += n;
        if ((uint64_t)n < iov[i].iov_len) {
            break;
        }
    }
    return total;
}

static long linux_readlink(const char *path, char *buf, size_t bufsiz)
{
    if (!path || !buf) {
        return -LINUX_EFAULT;
    }
    if (bufsiz == 0) {
        return -LINUX_EINVAL;
    }
    struct process *proc = process_current();
    const char *target = NULL;
    if (strcmp(path, "/proc/self/exe") == 0 || strcmp(path, "/proc/thread-self/exe") == 0) {
        target = proc && proc->linux_exec_path[0] ? proc->linux_exec_path : "/usr/linux/bin/unknown";
    } else if (strncmp(path, "/proc/self/fd/", 14) == 0) {
        int fd = atoi(path + 14);
        struct file_descriptor *file = process_get_fd(proc, fd);
        if (!file || !file->node) {
            return -LINUX_EBADF;
        }
        if (file->node->type == VFS_NODE_DEV) {
            static char dev_target[VFS_PATH_MAX];
            ksnprintf(dev_target, sizeof(dev_target), "/dev/%s", file->node->name);
            target = dev_target;
        } else {
            target = file->node->name;
        }
    }
    if (!target) {
        return -LINUX_ENOENT;
    }
    size_t n = strlen(target);
    if (n > bufsiz) {
        n = bufsiz;
    }
    memcpy(buf, target, n);
    return (long)n;
}

static long linux_access_path(const char *path, int mode)
{
    char resolved[VFS_PATH_MAX];
    if (linux_resolve_path(path, resolved, sizeof(resolved)) < 0) {
        return -LINUX_EFAULT;
    }
    return native_to_linux(native_dispatch(SYS_ACCESS, (uint64_t)(uintptr_t)resolved,
                                           (uint64_t)mode, 0, 0, 0, 0));
}

static long linux_chdir_path(const char *path)
{
    char resolved[VFS_PATH_MAX];
    if (linux_resolve_path(path, resolved, sizeof(resolved)) < 0) {
        return -LINUX_EFAULT;
    }
    return native_to_linux(native_dispatch(SYS_CHDIR, (uint64_t)(uintptr_t)resolved,
                                           0, 0, 0, 0, 0));
}

static long linux_getcwd(char *buf, size_t size)
{
    struct process *proc = process_current();
    if (!buf || size == 0 || !proc) {
        return -LINUX_EFAULT;
    }
    size_t len = strlen(proc->cwd) + 1;
    if (len > size) {
        return -LINUX_ERANGE;
    }
    memcpy(buf, proc->cwd, len);
    return (long)len;
}

static long linux_getrlimit(int resource, struct linux_rlimit64 *out)
{
    (void)resource;
    if (!out) {
        return -LINUX_EFAULT;
    }
    out->rlim_cur = 64ULL * 1024ULL * 1024ULL;
    out->rlim_max = 64ULL * 1024ULL * 1024ULL;
    return 0;
}

static long linux_sysinfo(struct linux_sysinfo *out)
{
    if (!out) {
        return -LINUX_EFAULT;
    }
    const struct memory_stats *mem = memory_stats_get();
    memset(out, 0, sizeof(*out));
    out->uptime = (int64_t)((uint64_t)native_dispatch(SYS_UPTIME_MS, 0, 0, 0, 0, 0, 0) / 1000);
    out->totalram = mem ? mem->total_bytes : 0;
    out->freeram = mem ? mem->usable_bytes : 0;
    out->mem_unit = 1;
    out->procs = PROCESS_MAX;
    return 0;
}

static long linux_fcntl(int fd, int cmd, uint64_t arg)
{
    enum {
        LINUX_F_DUPFD = 0,
        LINUX_F_GETFD = 1,
        LINUX_F_SETFD = 2,
        LINUX_F_GETFL = 3,
        LINUX_F_SETFL = 4,
    };

    struct process *proc = process_current();
    struct file_descriptor *file = process_get_fd(proc, fd);
    if (!file) {
        return -LINUX_EBADF;
    }
    switch (cmd) {
    case LINUX_F_DUPFD:
        (void)arg;
        return native_to_linux(native_dispatch(SYS_DUP, (uint64_t)fd, 0, 0, 0, 0, 0));
    case LINUX_F_GETFD:
        return 0;
    case LINUX_F_SETFD:
        return 0;
    case LINUX_F_GETFL:
        return file->flags;
    case LINUX_F_SETFL:
        file->flags = (file->flags & ~(VFS_O_APPEND | VFS_O_NONBLOCK)) |
                      linux_open_flags_to_vfs((int)arg);
        return 0;
    default:
        return -LINUX_EINVAL;
    }
}

static long linux_getpgid(int pid)
{
    struct process *proc = pid ? process_find(pid) : process_current();
    return proc ? proc->pgid : -LINUX_ESRCH;
}

static long linux_setuid(uint32_t uid)
{
    struct process *proc = process_current();
    if (!proc) {
        return -LINUX_ESRCH;
    }
    if (proc->uid != 0 && proc->uid != uid) {
        return -LINUX_EPERM;
    }
    proc->uid = uid;
    return 0;
}

static long linux_setgid(uint32_t gid)
{
    struct process *proc = process_current();
    if (!proc) {
        return -LINUX_ESRCH;
    }
    if (proc->uid != 0 && proc->gid != gid) {
        return -LINUX_EPERM;
    }
    proc->gid = gid;
    return 0;
}

static unsigned char linux_dtype_from_node(const struct vfs_node *node)
{
    if (!node) {
        return LINUX_DT_UNKNOWN;
    }
    if (node->type == VFS_NODE_DIR) {
        return LINUX_DT_DIR;
    }
    if (node->type == VFS_NODE_DEV) {
        return LINUX_DT_CHR;
    }
    if (node->type == VFS_NODE_FILE || node->type == VFS_NODE_PROC) {
        return LINUX_DT_REG;
    }
    return LINUX_DT_UNKNOWN;
}

static void linux_getdents_emit(struct vfs_node *node, void *opaque)
{
    struct linux_dirent_ctx *ctx = opaque;
    if (ctx->index++ < ctx->start || !node) {
        return;
    }

    size_t name_len = strlen(node->name);
    size_t reclen = 19 + name_len + 1;
    reclen = (reclen + 7) & ~(size_t)7;
    if (ctx->written + reclen > ctx->size) {
        return;
    }

    char *p = ctx->buf + ctx->written;
    uint64_t ino = (uint64_t)(uintptr_t)node;
    int64_t off = (int64_t)(ctx->index);
    uint16_t r = (uint16_t)reclen;
    memcpy(p, &ino, sizeof(ino));
    memcpy(p + 8, &off, sizeof(off));
    memcpy(p + 16, &r, sizeof(r));
    p[18] = (char)linux_dtype_from_node(node);
    memcpy(p + 19, node->name, name_len + 1);
    if (reclen > 19 + name_len + 1) {
        memset(p + 19 + name_len + 1, 0, reclen - (19 + name_len + 1));
    }

    ctx->written += reclen;
    ctx->emitted++;
}

static long linux_getdents64(int fd, void *buf, size_t count)
{
    struct process *proc = process_current();
    struct file_descriptor *file = process_get_fd(proc, fd);
    if (!file) {
        return -LINUX_EBADF;
    }
    if (!buf && count) {
        return -LINUX_EFAULT;
    }
    if (file->node->type != VFS_NODE_DIR) {
        return -LINUX_ENOTDIR;
    }
    struct linux_dirent_ctx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.buf = buf;
    ctx.size = count;
    ctx.start = file->offset;
    vfs_list(file->node, linux_getdents_emit, &ctx);
    file->offset += (uint64_t)ctx.emitted;
    return (long)ctx.written;
}

static long linux_uname(struct linux_utsname *out)
{
    if (!out) {
        return -LINUX_EFAULT;
    }
    memset(out, 0, sizeof(*out));
    strncpy(out->sysname, "Linux", sizeof(out->sysname) - 1);
    strncpy(out->nodename, "tiramisu", sizeof(out->nodename) - 1);
    strncpy(out->release, "5.10.0-tnu", sizeof(out->release) - 1);
    strncpy(out->version, "Tiramisu Linux ABI compatibility", sizeof(out->version) - 1);
    strncpy(out->machine, "x86_64", sizeof(out->machine) - 1);
    strncpy(out->domainname, "local", sizeof(out->domainname) - 1);
    return 0;
}

static long linux_clock_gettime(int clockid, struct linux_timespec *tp)
{
    (void)clockid;
    if (!tp) {
        return -LINUX_EFAULT;
    }
    uint64_t ms = (uint64_t)native_dispatch(SYS_UPTIME_MS, 0, 0, 0, 0, 0, 0);
    tp->tv_sec = (int64_t)(ms / 1000);
    tp->tv_nsec = (int64_t)((ms % 1000) * 1000000);
    return 0;
}

static long linux_gettimeofday(struct linux_timeval *tv, void *tz)
{
    (void)tz;
    if (!tv) {
        return 0;
    }
    uint64_t ms = (uint64_t)native_dispatch(SYS_UPTIME_MS, 0, 0, 0, 0, 0, 0);
    tv->tv_sec = (int64_t)(ms / 1000);
    tv->tv_usec = (int64_t)((ms % 1000) * 1000);
    return 0;
}

static long linux_time(int64_t *out)
{
    uint64_t ms = (uint64_t)native_dispatch(SYS_UPTIME_MS, 0, 0, 0, 0, 0, 0);
    int64_t sec = (int64_t)(ms / 1000);
    if (out) {
        *out = sec;
    }
    return sec;
}

static long linux_getrandom(void *buf, size_t len, unsigned int flags)
{
    (void)flags;
    if (!buf && len) {
        return -LINUX_EFAULT;
    }
    uint8_t *p = buf;
    uint64_t x = scheduler_ticks() ^ (uint64_t)(uintptr_t)buf ^ 0x9e3779b97f4a7c15ULL;
    for (size_t i = 0; i < len; i++) {
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        p[i] = (uint8_t)x;
    }
    return (long)len;
}

static long linux_brk(uintptr_t addr)
{
    if (!linux_brk_floor) {
        linux_mm_reset(0x10000000ULL, 0x20000000ULL, 0x40000000ULL);
    }
    if (addr == 0) {
        return (long)linux_brk_current;
    }
    uintptr_t next = linux_page_up(addr);
    if (next < linux_brk_floor || next >= linux_brk_limit) {
        return (long)linux_brk_current;
    }
    if (next > linux_brk_current &&
        vmm_map_range_identity(linux_brk_current, next - linux_brk_current,
                               VMM_FLAG_WRITABLE | VMM_FLAG_USER) < 0) {
        return (long)linux_brk_current;
    }
    linux_brk_current = next;
    return (long)linux_brk_current;
}

static long linux_mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset)
{
    (void)prot;

    if (length == 0) {
        return -LINUX_EINVAL;
    }
    if (!linux_mmap_next) {
        linux_mm_reset(0x10000000ULL, 0x20000000ULL, 0x40000000ULL);
    }

    size_t size = linux_page_up(length);
    uintptr_t base;
    if ((flags & LINUX_MAP_FIXED) && addr) {
        base = (uintptr_t)addr;
    } else {
        base = linux_mmap_next;
        linux_mmap_next += size;
    }
    if (base + size <= base || base + size > linux_mmap_limit) {
        return -LINUX_ENOMEM;
    }
    if (!(flags & (LINUX_MAP_ANON | LINUX_MAP_PRIVATE | LINUX_MAP_SHARED)) && fd < 0) {
        return -LINUX_EINVAL;
    }
    if (vmm_map_range_identity(base, size, VMM_FLAG_WRITABLE | VMM_FLAG_USER) < 0) {
        return -LINUX_ENOMEM;
    }
    if (flags & LINUX_MAP_ANON) {
        memset((void *)base, 0, size);
    } else if (fd >= 0) {
        struct process *proc = process_current();
        struct file_descriptor *file = process_get_fd(proc, fd);
        if (!file || !file->node || offset < 0) {
            return -LINUX_EBADF;
        }
        memset((void *)base, 0, size);
        ssize_t n = vfs_read_node(file->node, (uint64_t)offset, (void *)base, length);
        if (n < 0) {
            return -LINUX_EIO;
        }
    }
    return (long)base;
}

static long linux_arch_prctl(int code, uintptr_t addr)
{
    struct process *proc = process_current();
    if (!proc) {
        return -LINUX_ESRCH;
    }
    if (code == LINUX_ARCH_SET_FS) {
        proc->linux_tls_base = addr;
        cpu_set_fs_base(addr);
        return 0;
    }
    if (code == LINUX_ARCH_GET_FS) {
        if (!addr) {
            return -LINUX_EFAULT;
        }
        *(uintptr_t *)addr = proc->linux_tls_base;
        return 0;
    }
    return -LINUX_EINVAL;
}

static long linux_futex(uint32_t *uaddr, int op, uint32_t val,
                        const struct linux_timespec *timeout)
{
    (void)timeout;
    if (!uaddr) {
        return -LINUX_EFAULT;
    }
    int cmd = op & 0x7f;
    if (cmd == LINUX_FUTEX_WAKE) {
        return 0;
    }
    if (cmd == LINUX_FUTEX_WAIT) {
        return *uaddr == val ? -LINUX_EAGAIN : -LINUX_EAGAIN;
    }
    return -LINUX_ENOSYS;
}

static long linux_kill(int pid, int sig)
{
    (void)sig;
    return process_kill(pid) < 0 ? -LINUX_ESRCH : 0;
}

static long dispatch_native_syscall(uint64_t native, const struct linux_syscall_args *a)
{
    return native_to_linux(native_dispatch(native, a->a0, a->a1, a->a2,
                                          a->a3, a->a4, a->a5));
}

long linux_syscall_dispatch(const struct linux_syscall_args *a)
{
    if (!a || a->nr >= linux_syscall_table_size || !linux_syscall_table[a->nr].name) {
        return -LINUX_ENOSYS;
    }

    switch (a->nr) {
    case 0:
        return dispatch_native_syscall(SYS_READ, a);
    case 1:
        return dispatch_native_syscall(SYS_WRITE, a);
    case 2:
        return linux_open_path((const char *)(uintptr_t)a->a0, (int)a->a1, (int)a->a2);
    case 3:
        return dispatch_native_syscall(SYS_CLOSE, a);
    case 4:
    case 6:
        return linux_stat_path((const char *)(uintptr_t)a->a0,
                               (struct linux_stat *)(uintptr_t)a->a1);
    case 5:
        return linux_fstat((int)a->a0, (struct linux_stat *)(uintptr_t)a->a1);
    case 7:
        return dispatch_native_syscall(SYS_POLL, a);
    case 8:
        return dispatch_native_syscall(SYS_LSEEK, a);
    case 9:
        return linux_mmap((void *)(uintptr_t)a->a0, (size_t)a->a1, (int)a->a2,
                          (int)a->a3, (int)a->a4, (off_t)a->a5);
    case 10:
    case 11:
        return 0;
    case 12:
        return linux_brk((uintptr_t)a->a0);
    case 13:
        return dispatch_native_syscall(SYS_SIGACTION, a);
    case 16:
        return dispatch_native_syscall(SYS_IOCTL, a);
    case 17:
        return linux_pread64((int)a->a0, (void *)(uintptr_t)a->a1,
                             (size_t)a->a2, (off_t)a->a3);
    case 18:
        return linux_pwrite64((int)a->a0, (const void *)(uintptr_t)a->a1,
                              (size_t)a->a2, (off_t)a->a3);
    case 19:
        return linux_readv((int)a->a0, (const struct linux_iovec *)(uintptr_t)a->a1,
                           (int)a->a2);
    case 20:
        return linux_writev((int)a->a0, (const struct linux_iovec *)(uintptr_t)a->a1,
                            (int)a->a2);
    case 21:
        return linux_access_path((const char *)(uintptr_t)a->a0, (int)a->a1);
    case 23:
        return dispatch_native_syscall(SYS_SELECT, a);
    case 24:
    case 95:
    case 273:
        return 0;
    case 72:
        return linux_fcntl((int)a->a0, (int)a->a1, a->a2);
    case 74:
        return 0;
    case 32:
        return dispatch_native_syscall(SYS_DUP, a);
    case 33:
        return dispatch_native_syscall(SYS_DUP2, a);
    case 39:
    case 186:
        return dispatch_native_syscall(SYS_GETPID, a);
    case 59:
        return linux_execve((const char *)(uintptr_t)a->a0,
                            (char *const *)(uintptr_t)a->a1,
                            (char *const *)(uintptr_t)a->a2);
    case 60:
    case 231:
        return dispatch_native_syscall(SYS_EXIT, a);
    case 61:
        return dispatch_native_syscall(SYS_WAIT, a);
    case 62:
        return linux_kill((int)a->a0, (int)a->a1);
    case 63:
        return linux_uname((struct linux_utsname *)(uintptr_t)a->a0);
    case 78:
    case 217:
        return linux_getdents64((int)a->a0, (void *)(uintptr_t)a->a1, (size_t)a->a2);
    case 79:
        return linux_getcwd((char *)(uintptr_t)a->a0, (size_t)a->a1);
    case 80:
        return linux_chdir_path((const char *)(uintptr_t)a->a0);
    case 83:
        return linux_native_path_syscall(SYS_MKDIR, (const char *)(uintptr_t)a->a0,
                                         a->a1, 0);
    case 87:
        return linux_native_path_syscall(SYS_UNLINK, (const char *)(uintptr_t)a->a0,
                                         0, 0);
    case 90:
        return linux_native_path_syscall(SYS_CHMOD, (const char *)(uintptr_t)a->a0,
                                         a->a1, 0);
    case 92:
        return linux_native_path_syscall(SYS_CHOWN, (const char *)(uintptr_t)a->a0,
                                         a->a1, a->a2);
    case 89:
        return linux_readlink((const char *)(uintptr_t)a->a0,
                              (char *)(uintptr_t)a->a1, (size_t)a->a2);
    case 97:
        return linux_getrlimit((int)a->a0, (struct linux_rlimit64 *)(uintptr_t)a->a1);
    case 96:
        return linux_gettimeofday((struct linux_timeval *)(uintptr_t)a->a0,
                                  (void *)(uintptr_t)a->a1);
    case 99:
        return linux_sysinfo((struct linux_sysinfo *)(uintptr_t)a->a0);
    case 102:
    case 107:
        return dispatch_native_syscall(SYS_GETUID, a);
    case 104:
    case 108:
        return dispatch_native_syscall(SYS_GETGID, a);
    case 105:
        return linux_setuid((uint32_t)a->a0);
    case 106:
        return linux_setgid((uint32_t)a->a0);
    case 110:
        return dispatch_native_syscall(SYS_GETPPID, a);
    case 111:
        return linux_getpgid(0);
    case 115:
        return 0;
    case 121:
        return linux_getpgid((int)a->a0);
    case 131:
        return 0;
    case 158:
        return linux_arch_prctl((int)a->a0, (uintptr_t)a->a1);
    case 202:
        return linux_futex((uint32_t *)(uintptr_t)a->a0, (int)a->a1, (uint32_t)a->a2,
                           (const struct linux_timespec *)(uintptr_t)a->a3);
    case 201:
        return linux_time((int64_t *)(uintptr_t)a->a0);
    case 218:
        return dispatch_native_syscall(SYS_GETPID, a);
    case 228:
        return linux_clock_gettime((int)a->a0, (struct linux_timespec *)(uintptr_t)a->a1);
    case 230:
        return dispatch_native_syscall(SYS_NANOSLEEP, a);
    case 257:
        if ((int)a->a0 != AT_FDCWD) {
            return -LINUX_ENOTSUP;
        }
        return linux_open_path((const char *)(uintptr_t)a->a1, (int)a->a2, (int)a->a3);
    case 262:
        if ((a->a3 & AT_EMPTY_PATH) && a->a1) {
            const char *path = (const char *)(uintptr_t)a->a1;
            if (path[0] == '\0') {
                return linux_fstat((int)a->a0, (struct linux_stat *)(uintptr_t)a->a2);
            }
        }
        if ((int)a->a0 != AT_FDCWD) {
            return -LINUX_ENOTSUP;
        }
        return linux_stat_path((const char *)(uintptr_t)a->a1,
                               (struct linux_stat *)(uintptr_t)a->a2);
    case 263:
        if ((int)a->a0 != AT_FDCWD) {
            return -LINUX_ENOTSUP;
        }
        return linux_native_path_syscall(SYS_UNLINK, (const char *)(uintptr_t)a->a1,
                                         0, 0);
    case 267:
        if ((int)a->a0 != AT_FDCWD) {
            return -LINUX_ENOTSUP;
        }
        return linux_readlink((const char *)(uintptr_t)a->a1,
                              (char *)(uintptr_t)a->a2, (size_t)a->a3);
    case 302:
        if (a->a2) {
            return -LINUX_EPERM;
        }
        return linux_getrlimit((int)a->a1, (struct linux_rlimit64 *)(uintptr_t)a->a3);
    case 318:
        return linux_getrandom((void *)(uintptr_t)a->a0, (size_t)a->a1, (unsigned int)a->a2);
    case 322:
        return linux_execveat((int)a->a0, (const char *)(uintptr_t)a->a1,
                              (char *const *)(uintptr_t)a->a2,
                              (char *const *)(uintptr_t)a->a3, (int)a->a4);
    default:
        return -LINUX_ENOSYS;
    }
}

long linux_syscall_entry(uint64_t nr, uint64_t a0, uint64_t a1, uint64_t a2,
                         uint64_t a3, uint64_t a4, uint64_t a5)
{
    struct linux_syscall_args args = { nr, a0, a1, a2, a3, a4, a5 };
    return linux_syscall_dispatch(&args);
}

const char *linux_syscall_name(uint64_t nr)
{
    if (nr >= linux_syscall_table_size || !linux_syscall_table[nr].name) {
        return "unknown";
    }
    return linux_syscall_table[nr].name;
}
