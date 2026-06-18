/*
 * Linux signal handling for TNU
 *
 * Implements rt_sigaction, rt_sigprocmask, rt_sigreturn, sigaltstack for Linux
 * compatibility. Signals are minimally supported on TNU.
 */

#include <tnu/linux_compat.h>
#include <tnu/log.h>
#include <tnu/process.h>
#include <tnu/string.h>

#include "../syscall/linux_errno.h"

/* Linux signal numbers */
#define LINUX_SIGINT    2
#define LINUX_SIGQUIT   3
#define LINUX_SIGKILL   9
#define LINUX_SIGTERM  15

/* Signal action structures */
struct linux_sigaction {
    void (*sa_handler)(int);
    unsigned long sa_flags;
    void (*sa_restorer)(void);
    uint64_t sa_mask[1];
};

struct linux_sigset_t {
    uint64_t bits[1];
};

struct linux_stack_t {
    void *ss_sp;
    int ss_flags;
    size_t ss_size;
};

/*
 * rt_sigaction() - examine and change a signal action
 * For now, accept signal registration but don't actually install handlers.
 */
long linux_rt_sigaction(int signum, const void *act,
                        void *oldact, size_t sigsetsize)
{
    (void)signum;
    (void)act;
    (void)sigsetsize;

    if (oldact) {
        memset(oldact, 0, sizeof(struct linux_sigaction));
    }
    /* Pretend success - signals aren't fully wired yet */
    return 0;
}

/*
 * rt_sigprocmask() - examine and change blocked signals
 */
long linux_rt_sigprocmask(int how, const void *set,
                          void *oldset, size_t sigsetsize)
{
    (void)how;
    (void)set;
    (void)sigsetsize;

    if (oldset) {
        memset(oldset, 0, sizeof(struct linux_sigset_t));
    }
    return 0;
}

/*
 * rt_sigreturn() - return from signal handler and cleanup stack frame
 */
long linux_rt_sigreturn(void)
{
    /* Not implemented */
    return -LINUX_ENOSYS;
}

/*
 * sigaltstack() - set and/or get signal stack context
 */
long linux_sigaltstack(const void *ss, void *old_ss)
{
    (void)ss;

    if (old_ss) {
        memset(old_ss, 0, sizeof(struct linux_stack_t));
    }
    return 0;
}

void linux_signal_init(void)
{
    log_info("linux-compat", "Linux signal layer initialized (stubs only)");
}
