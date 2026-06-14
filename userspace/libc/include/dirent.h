#ifndef TNU_DIRENT_H
#define TNU_DIRENT_H

#include <sys/types.h>
#include <tnu/syscall.h>

#define DT_UNKNOWN 0
#define DT_DIR     4
#define DT_REG     8

int readdir_fd(int fd, struct syscall_dirent *out);

struct dirent {
    ino_t d_ino;
    unsigned char d_type;
    char d_name[256];
};

typedef struct DIR DIR;

DIR *opendir(const char *name);
struct dirent *readdir(DIR *dirp);
int closedir(DIR *dirp);

#endif
