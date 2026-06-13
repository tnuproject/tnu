#ifndef TNU_TIME_H
#define TNU_TIME_H

#include <sys/types.h>

typedef long clock_t;
struct timespec {
    time_t tv_sec;
    long tv_nsec;
};

#define CLOCKS_PER_SEC 1000

clock_t clock(void);
time_t time(time_t *tloc);

#endif
