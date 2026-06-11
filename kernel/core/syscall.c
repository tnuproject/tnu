#include <tnu/console.h>
#include <tnu/process.h>
#include <tnu/string.h>
#include <tnu/syscall.h>
#include <tnu/user.h>
#include <tnu/vfs.h>

static bool has_perm(const struct process *proc, const struct vfs_node *node, uint32_t perm)
{
    if (!proc || !node) {
        return false;
    }
    if (proc->uid == 0) {
        return true;
    }
    uint32_t shift = 0;
    if (proc->uid == node->uid) {
        shift = 6;
    } else if (proc->gid == node->gid) {
        shift = 3;
    }
    return ((node->mode >> shift) & perm) == perm;
}

static long sys_open(const char *path, int flags, int mode)
{
    struct process *proc = process_current();
    struct vfs_node *node = vfs_lookup(path, proc->cwd);
    if (!node && (flags & VFS_O_CREAT)) {
        if (vfs_create_file(path, proc->cwd, VFS_S_IFREG | (mode & 0777), proc->uid, proc->gid) < 0) {
            return -1;
        }
        node = vfs_lookup(path, proc->cwd);
    }
    if (!node) {
        return -1;
    }
    bool wants_write = (flags & VFS_O_WRONLY) || (flags & VFS_O_RDWR) || (flags & VFS_O_TRUNC);
    bool wants_read = !wants_write || (flags & VFS_O_RDWR);
    if ((wants_read && !has_perm(proc, node, 4)) ||
        (wants_write && !has_perm(proc, node, 2))) {
        return -1;
    }
    return process_open_fd(proc, node, flags);
}

static long sys_read(int fd, void *buf, size_t count)
{
    struct process *proc = process_current();
    if (fd == 0) {
        char *cbuf = buf;
        for (size_t i = 0; i < count; i++) {
            cbuf[i] = (char)console_getchar();
            if (cbuf[i] == '\n') {
                return (long)i + 1;
            }
        }
        return (long)count;
    }
    struct file_descriptor *file = process_get_fd(proc, fd);
    if (!file) {
        return -1;
    }
    if (file->flags & VFS_O_WRONLY) {
        return -1;
    }
    ssize_t ret = vfs_read_node(file->node, file->offset, buf, count);
    if (ret > 0) {
        file->offset += (uint64_t)ret;
    }
    return ret;
}

static long sys_write(int fd, const void *buf, size_t count)
{
    struct process *proc = process_current();
    if (fd == 1 || fd == 2) {
        console_write_n(buf, count);
        return (long)count;
    }
    struct file_descriptor *file = process_get_fd(proc, fd);
    if (!file) {
        return -1;
    }
    if (!(file->flags & VFS_O_WRONLY) && !(file->flags & VFS_O_RDWR)) {
        return -1;
    }
    ssize_t ret = vfs_write_node(file->node, file->offset, buf, count);
    if (ret > 0) {
        file->offset += (uint64_t)ret;
    }
    return ret;
}

static long sys_lseek(int fd, int64_t offset, int whence)
{
    struct process *proc = process_current();
    struct file_descriptor *file = process_get_fd(proc, fd);
    if (!file) {
        return -1;
    }

    int64_t base = 0;
    switch (whence) {
    case 0:
        base = 0;
        break;
    case 1:
        base = (int64_t)file->offset;
        break;
    case 2:
        base = (int64_t)file->node->size;
        break;
    default:
        return -1;
    }

    int64_t next = base + offset;
    if (next < 0) {
        return -1;
    }
    file->offset = (uint64_t)next;
    return next;
}

static long sys_access(const char *path, int mode)
{
    struct process *proc = process_current();
    if (!proc || !path || (mode & ~(1 | 2 | 4)) != 0) {
        return -1;
    }
    struct vfs_node *node = vfs_lookup(path, proc->cwd);
    if (!node) {
        return -1;
    }
    if (mode == 0) {
        return 0;
    }
    return has_perm(proc, node, (uint32_t)mode) ? 0 : -1;
}

long syscall_dispatch(uint64_t number, uint64_t a0, uint64_t a1, uint64_t a2,
                      uint64_t a3, uint64_t a4, uint64_t a5)
{
    (void)a3;
    (void)a4;
    (void)a5;
    struct process *proc = process_current();

    switch (number) {
    case SYS_READ:
        return sys_read((int)a0, (void *)a1, (size_t)a2);
    case SYS_WRITE:
        return sys_write((int)a0, (const void *)a1, (size_t)a2);
    case SYS_OPEN:
        return sys_open((const char *)a0, (int)a1, (int)a2);
    case SYS_CLOSE:
        process_close_fd(proc, (int)a0);
        return 0;
    case SYS_GETPID:
        return proc ? proc->pid : -1;
    case SYS_GETPPID:
        return proc ? proc->ppid : -1;
    case SYS_GETUID:
        return proc ? (long)proc->uid : -1;
    case SYS_GETGID:
        return proc ? (long)proc->gid : -1;
    case SYS_LSEEK:
        return sys_lseek((int)a0, (int64_t)a1, (int)a2);
    case SYS_ACCESS:
        return sys_access((const char *)a0, (int)a1);
    case SYS_DUP:
        return process_dup_fd(proc, (int)a0);
    case SYS_DUP2:
        return process_dup2_fd(proc, (int)a0, (int)a1);
    case SYS_CHDIR: {
        struct vfs_node *node = vfs_lookup((const char *)a0, proc->cwd);
        if (!node || node->type != VFS_NODE_DIR || !has_perm(proc, node, 1)) {
            return -1;
        }
        return vfs_normalize((const char *)a0, proc->cwd, proc->cwd, sizeof(proc->cwd));
    }
    case SYS_GETCWD:
        strncpy((char *)a0, proc->cwd, (size_t)a1);
        return 0;
    case SYS_MKDIR:
        return vfs_mkdir((const char *)a0, proc->cwd, VFS_S_IFDIR | ((uint32_t)a1 & 0777),
                         proc->uid, proc->gid);
    case SYS_UNLINK:
    {
        struct vfs_node *node = vfs_lookup((const char *)a0, proc->cwd);
        if (!node || (proc->uid != 0 && proc->uid != node->uid)) {
            return -1;
        }
        return vfs_unlink((const char *)a0, proc->cwd);
    }
    case SYS_STAT:
        return vfs_stat((const char *)a0, proc->cwd, (struct vfs_stat *)a1);
    case SYS_CHMOD:
    {
        struct vfs_node *node = vfs_lookup((const char *)a0, proc->cwd);
        if (!node || (proc->uid != 0 && proc->uid != node->uid)) {
            return -1;
        }
        return vfs_chmod((const char *)a0, proc->cwd, (uint32_t)a1);
    }
    case SYS_CHOWN:
        if (proc->uid != 0) {
            return -1;
        }
        return vfs_chown((const char *)a0, proc->cwd, (uint32_t)a1, (uint32_t)a2);
    case SYS_SPAWN: {
        const struct user_record *u = user_current();
        struct process *child = process_create((const char *)a0, proc ? proc->pid : 0, u->uid, u->gid);
        return child ? child->pid : -1;
    }
    case SYS_EXEC:
        return -1;
    case SYS_WAIT:
        return -1;
    case SYS_EXIT:
        process_exit(proc, (int)a0);
        return 0;
    default:
        return -1;
    }
}
