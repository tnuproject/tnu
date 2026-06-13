#include <tnu/process.h>
#include <tnu/string.h>
#include <tnu/user.h>

static struct process processes[PROCESS_MAX];
static int next_pid = 1;
static struct process *current;

static void setup_stdio(struct process *proc)
{
    struct vfs_node *tty = vfs_lookup("/dev/tty", "/");
    for (int i = 0; i < 3; i++) {
        proc->fds[i].used = true;
        proc->fds[i].flags = i == 0 ? VFS_O_RDONLY : VFS_O_WRONLY;
        proc->fds[i].node = tty;
        proc->fds[i].offset = 0;
    }
}

void process_init(void)
{
    memset(processes, 0, sizeof(processes));
    next_pid = 1;
    const struct user_record *u = user_current();
    struct process *init = process_create("init", 0, u->uid, u->gid);
    if (init) {
        init->state = PROCESS_READY;
        struct process *shell = process_create("tsh", init->pid, u->uid, u->gid);
        if (shell) {
            shell->state = PROCESS_RUNNING;
            current = shell;
        } else {
            init->state = PROCESS_RUNNING;
            current = init;
        }
    }
}

struct process *process_current(void)
{
    return current;
}

struct process *process_create(const char *name, int ppid, uint32_t uid, uint32_t gid)
{
    for (size_t i = 0; i < PROCESS_MAX; i++) {
        if (processes[i].state == PROCESS_UNUSED) {
            struct process *p = &processes[i];
            memset(p, 0, sizeof(*p));
            p->pid = next_pid++;
            p->ppid = ppid;
            p->uid = uid;
            p->gid = gid;
            p->state = PROCESS_READY;
            strncpy(p->name, name, PROCESS_NAME_MAX);
            const struct user_record *user = user_find_uid(uid);
            if (user && user->home[0]) {
                strncpy(p->cwd, user->home, sizeof(p->cwd) - 1);
                p->cwd[sizeof(p->cwd) - 1] = '\0';
            } else {
                strcpy(p->cwd, "/");
            }
            setup_stdio(p);
            return p;
        }
    }
    return NULL;
}

struct process *process_find(int pid)
{
    for (size_t i = 0; i < PROCESS_MAX; i++) {
        if (processes[i].state != PROCESS_UNUSED && processes[i].pid == pid) {
            return &processes[i];
        }
    }
    return NULL;
}

void process_exit(struct process *proc, int code)
{
    if (!proc) {
        return;
    }
    proc->exit_code = code;
    proc->state = PROCESS_ZOMBIE;
}

int process_kill(int pid)
{
    struct process *p = process_find(pid);
    if (!p || p->pid == 1) {
        return -1;
    }
    struct process *caller = process_current();
    if (!caller || (caller->uid != 0 && caller->uid != p->uid)) {
        return -1;
    }
    process_exit(p, -1);
    return 0;
}

void process_each(void (*visitor)(const struct process *proc, void *ctx), void *ctx)
{
    for (size_t i = 0; i < PROCESS_MAX; i++) {
        if (processes[i].state != PROCESS_UNUSED) {
            visitor(&processes[i], ctx);
        }
    }
}

const char *process_state_name(enum process_state state)
{
    switch (state) {
    case PROCESS_READY:
        return "ready";
    case PROCESS_RUNNING:
        return "running";
    case PROCESS_SLEEPING:
        return "sleep";
    case PROCESS_ZOMBIE:
        return "zombie";
    default:
        return "unused";
    }
}

int process_open_fd(struct process *proc, struct vfs_node *node, int flags)
{
    if (!proc || !node) {
        return -1;
    }
    for (int i = 3; i < VFS_MAX_FDS; i++) {
        if (!proc->fds[i].used) {
            proc->fds[i].used = true;
            proc->fds[i].flags = flags;
            proc->fds[i].offset = (flags & VFS_O_APPEND) ? node->size : 0;
            proc->fds[i].node = node;
            return i;
        }
    }
    return -1;
}

struct file_descriptor *process_get_fd(struct process *proc, int fd)
{
    if (!proc || fd < 0 || fd >= VFS_MAX_FDS || !proc->fds[fd].used) {
        return NULL;
    }
    return &proc->fds[fd];
}

void process_close_fd(struct process *proc, int fd)
{
    if (!proc || fd < 0 || fd >= VFS_MAX_FDS || fd < 3) {
        return;
    }
    memset(&proc->fds[fd], 0, sizeof(proc->fds[fd]));
}

int process_dup_fd(struct process *proc, int oldfd)
{
    struct file_descriptor *old = process_get_fd(proc, oldfd);
    if (!old) {
        return -1;
    }
    for (int fd = 3; fd < VFS_MAX_FDS; fd++) {
        if (!proc->fds[fd].used) {
            proc->fds[fd] = *old;
            return fd;
        }
    }
    return -1;
}

int process_dup2_fd(struct process *proc, int oldfd, int newfd)
{
    struct file_descriptor *old = process_get_fd(proc, oldfd);
    if (!proc || !old || newfd < 0 || newfd >= VFS_MAX_FDS) {
        return -1;
    }
    if (oldfd == newfd) {
        return newfd;
    }
    proc->fds[newfd] = *old;
    return newfd;
}
