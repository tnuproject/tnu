#ifndef TNU_SYS_STAT_H
#define TNU_SYS_STAT_H

#include <sys/types.h>

#define S_IFMT   0170000
#define S_IFIFO  0010000
#define S_IFCHR  0020000
#define S_IFDIR  0040000
#define S_IFBLK  0060000
#define S_IFREG  0100000
#define S_IFLNK  0120000
#define S_IFSOCK 0140000
#define S_IFPROC 0010000

#define S_IRWXU 0700
#define S_IRUSR 0400
#define S_IWUSR 0200
#define S_IXUSR 0100
#define S_IRWXG 0070
#define S_IRGRP 0040
#define S_IWGRP 0020
#define S_IXGRP 0010
#define S_IRWXO 0007
#define S_IROTH 0004
#define S_IWOTH 0002
#define S_IXOTH 0001

#define S_ISDIR(m)  (((m) & S_IFMT) == S_IFDIR)
#define S_ISREG(m)  (((m) & S_IFMT) == S_IFREG)
#define S_ISCHR(m)  (((m) & S_IFMT) == S_IFCHR)
#define S_ISBLK(m)  (((m) & S_IFMT) == S_IFBLK)
#define S_ISFIFO(m) (((m) & S_IFMT) == S_IFIFO)
#define S_ISLNK(m)  (((m) & S_IFMT) == S_IFLNK)
#define S_ISSOCK(m) (((m) & S_IFMT) == S_IFSOCK)
#define S_ISPROC(m) (((m) & S_IFMT) == S_IFPROC)

enum tnu_node_type {
    TNU_DT_DIR = 1,
    TNU_DT_FILE = 2,
    TNU_DT_DEV = 3,
    TNU_DT_PROC = 4,
};

#include <time.h>

struct stat {
    mode_t  st_mode;
    uid_t   st_uid;
    gid_t   st_gid;
    off_t   st_size;
    time_t  st_mtime;
    time_t  st_atime;
    time_t  st_ctime;
    dev_t   st_dev;
    ino_t   st_ino;
    nlink_t st_nlink;
    blksize_t st_blksize;
    blkcnt_t  st_blocks;
    /* POSIX.1-2008 nanosecond timestamps */
    struct timespec st_atim;
    struct timespec st_mtim;
    struct timespec st_ctim;
    int     st_type;
};

int stat(const char *path, struct stat *st);
int lstat(const char *path, struct stat *st);
int fstat(int fd, struct stat *st);
int mkdir(const char *path, mode_t mode);
int chmod(const char *path, mode_t mode);
int chown(const char *path, uid_t uid, gid_t gid);
int futimens(int fd, const struct timespec times[2]);

#endif
