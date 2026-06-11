#ifndef TNU_SIGNAL_H
#define TNU_SIGNAL_H

typedef int sig_atomic_t;

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

#define SIG_DFL ((void (*)(int))0)
#define SIG_IGN ((void (*)(int))1)
#define SIG_ERR ((void (*)(int))-1)

int raise(int sig);
void (*signal(int sig, void (*handler)(int)))(int);

#endif
