#ifndef TNU_DEV_H
#define TNU_DEV_H

#include <tnu/printf.h>
#include <tnu/string.h>
#include <tnu/syscall.h>
#include <tnu/types.h>
#include <tnu/vfs.h>

#ifndef EFAULT
#define EFAULT 14
#endif

#ifndef O_RDONLY
#define O_RDONLY VFS_O_RDONLY
#endif

#ifndef O_WRONLY
#define O_WRONLY VFS_O_WRONLY
#endif

#ifndef O_RDWR
#define O_RDWR VFS_O_RDWR
#endif

#ifndef O_CREAT
#define O_CREAT VFS_O_CREAT
#endif

#ifndef O_TRUNC
#define O_TRUNC VFS_O_TRUNC
#endif

#ifndef O_APPEND
#define O_APPEND VFS_O_APPEND
#endif

#ifndef O_EXCL
#define O_EXCL VFS_O_EXCL
#endif

#ifndef O_NONBLOCK
#define O_NONBLOCK VFS_O_NONBLOCK
#endif

#ifndef ENOTTY
#define ENOTTY 25
#endif

#ifndef __user
#define __user
#endif

struct stat {
    uint64_t st_size;
};

struct file {
    void *private_data;
};

struct cdev {
    void *private_data;
};

struct file_operations {
    void *owner;
    long (*unlocked_ioctl)(struct file *file, unsigned int cmd, unsigned long arg);
};

#define THIS_MODULE ((void *)0)

#define container_of(ptr, type, member) \
    ((type *)((char *)(ptr) - (char *)&((type *)0)->member))

static inline int sys_open(const char *path, int flags, unsigned int mode)
{
    return (int)syscall_dispatch(SYS_OPEN, (uint64_t)(uintptr_t)path, (uint64_t)flags,
                                 (uint64_t)mode, 0, 0, 0);
}

static inline int sys_close(int fd)
{
    return (int)syscall_dispatch(SYS_CLOSE, (uint64_t)fd, 0, 0, 0, 0, 0);
}

static inline ssize_t sys_read(int fd, void *buf, size_t count)
{
    return (ssize_t)syscall_dispatch(SYS_READ, (uint64_t)fd, (uint64_t)(uintptr_t)buf,
                                     (uint64_t)count, 0, 0, 0);
}

static inline int sys_fstat(int fd, struct stat *st)
{
    return (int)syscall_dispatch(SYS_FSTAT, (uint64_t)fd, (uint64_t)(uintptr_t)st,
                                 0, 0, 0, 0);
}

static inline long copy_to_user(void *dst, const void *src, size_t len)
{
    memcpy(dst, src, len);
    return 0;
}

static inline long copy_from_user(void *dst, const void *src, size_t len)
{
    memcpy(dst, src, len);
    return 0;
}

static inline int cdev_init(struct cdev *cdev, const struct file_operations *fops,
                            const char *name, unsigned int mode)
{
    (void)cdev;
    (void)fops;
    (void)name;
    (void)mode;
    return 0;
}

#endif
