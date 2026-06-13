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

#define SIG_DFL ((void (*)(int))0)
#define SIG_IGN ((void (*)(int))1)
#define SIG_ERR ((void (*)(int))-1)

int raise(int sig);
int kill(pid_t pid, int sig);
int sigemptyset(sigset_t *set);
int sigaction(int sig, const struct sigaction *act, struct sigaction *oldact);
void (*signal(int sig, void (*handler)(int)))(int);

#endif
