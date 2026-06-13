#ifndef TNU_TIME_H
#define TNU_TIME_H

#include <tnu/types.h>

struct rtc_time {
    int year;
    int month;
    int day;
    int hour;
    int minute;
    int second;
};

int rtc_read_time(struct rtc_time *out);
void time_init(void);
uint64_t time_now_seconds(void);
uint64_t time_uptime_seconds(void);

#endif
