#ifndef TNU_PROCESS_H
#define TNU_PROCESS_H

#include <tnu/types.h>
#include <tnu/vfs.h>

#define PROCESS_NAME_MAX 31
#define PROCESS_MAX 64
#define PROCESS_SIGNAL_MAX 32

enum process_state {
    PROCESS_UNUSED = 0,
    PROCESS_READY,
    PROCESS_RUNNING,
    PROCESS_SLEEPING,
    PROCESS_ZOMBIE,
};

struct process {
    int pid;
    int ppid;
    uint32_t uid;
    uint32_t gid;
    enum process_state state;
    char name[PROCESS_NAME_MAX + 1];
    char cwd[VFS_PATH_MAX];
    uint64_t started_ticks;
    int exit_code;
    uint8_t signal_disposition[PROCESS_SIGNAL_MAX];
    uintptr_t signal_handler[PROCESS_SIGNAL_MAX];
    struct file_descriptor fds[VFS_MAX_FDS];
};

void process_init(void);
struct process *process_current(void);
struct process *process_create(const char *name, int ppid, uint32_t uid, uint32_t gid);
struct process *process_find(int pid);
void process_exit(struct process *proc, int code);
int process_kill(int pid);
void process_each(void (*visitor)(const struct process *proc, void *ctx), void *ctx);
const char *process_state_name(enum process_state state);
int process_open_fd(struct process *proc, struct vfs_node *node, int flags);
struct file_descriptor *process_get_fd(struct process *proc, int fd);
void process_close_fd(struct process *proc, int fd);
int process_dup_fd(struct process *proc, int oldfd);
int process_dup2_fd(struct process *proc, int oldfd, int newfd);

#endif
