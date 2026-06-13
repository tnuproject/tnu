#ifndef TNU_UNISTD_H
#define TNU_UNISTD_H

#include <stddef.h>
#include <sys/types.h>
#include <tnu/syscall.h>

#define STDIN_FILENO  0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2

#define F_OK 0
#define X_OK 1
#define W_OK 2
#define R_OK 4

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

int access(const char *path, int mode);
int dup(int oldfd);
int dup2(int oldfd, int newfd);
int execl(const char *path, const char *arg, ...);
int execv(const char *path, char *const argv[]);
int execvp(const char *file, char *const argv[]);
pid_t fork(void);
int pipe(int pipefd[2]);
long fpathconf(int fd, int name);
char *getenv(const char *name);
int gethostname(char *name, size_t len);
uid_t geteuid(void);
int getppid(void);
int isatty(int fd);

#define _PC_PIPE_BUF 1

#endif
