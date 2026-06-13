#ifndef TNU_SIGNAL_H
#define TNU_SIGNAL_H

#include <sys/types.h>

typedef int sig_atomic_t;
typedef unsigned long sigset_t;

struct sigaction {
    union {
        void (*sa_handler)(int);
        void (*sa_sigaction)(int, void *, void *);
    };
    sigset_t sa_mask;
    int sa_flags;
};

#define SIGHUP  1
#define SIGINT  2
#define SIGQUIT 3
#define SIGILL  4
#define SIGABRT 6
#define SIGKILL 9
#define SIGSEGV 11
#define SIGPIPE 13
#define SIGALRM 14
#define SIGTERM 15
#define SIGSTOP 17
#define SIGWINCH 28

#define SIG_DFL  ((void (*)(int))0)
#define SIG_IGN  ((void (*)(int))1)
#define SIG_ERR  ((void (*)(int))-1)

#define SA_RESTART    0x10000000
#define SA_RESETHAND  0x80000000
#define SA_NOCLDSTOP  0x00000001
#define SA_NODEFER    0x40000000
#define SA_SIGINFO    0x00000004

#define SIG_BLOCK   0
#define SIG_UNBLOCK 1
#define SIG_SETMASK 2

int raise(int sig);
int kill(pid_t pid, int sig);
int sigemptyset(sigset_t *set);
int sigfillset(sigset_t *set);
int sigaddset(sigset_t *set, int signum);
int sigdelset(sigset_t *set, int signum);
int sigismember(const sigset_t *set, int signum);
int sigprocmask(int how, const sigset_t *set, sigset_t *oldset);
int sigaction(int sig, const struct sigaction *act, struct sigaction *oldact);
void (*signal(int sig, void (*handler)(int)))(int);

#endif
