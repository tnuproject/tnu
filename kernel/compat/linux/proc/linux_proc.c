/*
 * Linux signal handling for TNU
 *
 * Implements fork(), clone(), vfork() stubs for Linux compatibility.
 * Full process cloning is not yet implemented on TNU.
 */

#include <tnu/linux_compat.h>
#include <tnu/log.h>
#include <tnu/process.h>

#include "../syscall/linux_errno.h"

/*
 * fork() - create a child process
 * Not implemented: TNU currently only supports single-process execution.
 */
long linux_fork(void)
{
    return -LINUX_ENOSYS;
}

/*
 * vfork() - create a child process and block parent
 * Not implemented.
 */
long linux_vfork(void)
{
    return -LINUX_ENOSYS;
}

/*
 * clone() - create a child process with fine-grained control
 * Not implemented.
 */
long linux_clone(unsigned long flags, void *stack, int *parent_tid,
                 int *child_tid, unsigned long tls)
{
    (void)flags;
    (void)stack;
    (void)parent_tid;
    (void)child_tid;
    (void)tls;
    return -LINUX_ENOSYS;
}

void linux_proc_init(void)
{
    log_info("linux-compat", "Linux process layer initialized (stubs only)");
}
