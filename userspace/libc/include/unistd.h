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
int getppid(void);

#endif
