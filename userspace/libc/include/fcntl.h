#ifndef TNU_FCNTL_H
#define TNU_FCNTL_H

#ifndef O_RDONLY
#define O_RDONLY 0x0
#endif
#ifndef O_WRONLY
#define O_WRONLY 0x1
#endif
#ifndef O_RDWR
#define O_RDWR   0x2
#endif
#ifndef O_CREAT
#define O_CREAT  0x40
#endif
#ifndef O_TRUNC
#define O_TRUNC  0x200
#endif

int open(const char *path, int flags, ...);

#endif
