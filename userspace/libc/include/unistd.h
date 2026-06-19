#ifndef TNU_UNISTD_H
#define TNU_UNISTD_H

#include <stddef.h>
#include <stdint.h>
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
int exec(const char *path);
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
/* file IO */
ssize_t read(int fd, void *buf, size_t count);
ssize_t write(int fd, const void *buf, size_t count);
int close(int fd);
off_t lseek(int fd, off_t offset, int whence);
long tnu_syscall(long n, long a0, long a1, long a2, long a3, long a4, long a5);

/* process */
int spawn(const char *path);

int getpid(void);
int getppid(void);
int getuid(void);
int getgid(void);

int wait(int pid);

uid_t geteuid(void);
gid_t getegid(void);

int wait(int pid);

/* filesystem */
int chdir(const char *path);
int getcwd(char *buf, size_t size);
int unlink(const char *path);
int mkdir(const char *path, mode_t mode);
int rmdir(const char *path);
/* timing */
uint64_t uptime_ms(void);
unsigned int sleep(unsigned int seconds);
int usleep(unsigned long usec);

/* memory */
void *sbrk(intptr_t increment);

/* sync — flush all pending filesystem writes to disk.
 * On TNU this flushes the persistent TFS image. */
int sync(void);
int fsync(int fd);
int fdatasync(int fd);

/* Power management - requires root privileges */
int shutdown(void);
int reboot(void);

#define _PC_PIPE_BUF 1

void _exit(int status) __attribute__((noreturn));

#endif
