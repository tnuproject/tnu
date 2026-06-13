#include <arch/io.h>
#include <arch/pit.h>
#include <tnu/time.h>

static uint64_t boot_epoch;

static uint8_t cmos_read(uint8_t reg)
{
    outb(0x70, reg);
    return inb(0x71);
}

static int bcd_to_binary(int value)
{
    return (value & 0x0f) + ((value >> 4) * 10);
}

int rtc_read_time(struct rtc_time *out)
{
    if (!out) {
        return -1;
    }
    while (cmos_read(0x0a) & 0x80) {
    }

    int second = cmos_read(0x00);
    int minute = cmos_read(0x02);
    int hour = cmos_read(0x04);
    int day = cmos_read(0x07);
    int month = cmos_read(0x08);
    int year = cmos_read(0x09);
    int status_b = cmos_read(0x0b);

    if (!(status_b & 0x04)) {
        second = bcd_to_binary(second);
        minute = bcd_to_binary(minute);
        hour = bcd_to_binary(hour & 0x7f) | (hour & 0x80);
        day = bcd_to_binary(day);
        month = bcd_to_binary(month);
        year = bcd_to_binary(year);
    }
    if (!(status_b & 0x02) && (hour & 0x80)) {
        hour = ((hour & 0x7f) + 12) % 24;
    }

    out->year = 2000 + year;
    out->month = month;
    out->day = day;
    out->hour = hour;
    out->minute = minute;
    out->second = second;
    return 0;
}

static bool leap_year(int year)
{
    return (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
}

static int days_in_month(int year, int month)
{
    static const int days[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    if (month == 2 && leap_year(year)) {
        return 29;
    }
    return days[month - 1];
}

static uint64_t rtc_to_epoch(const struct rtc_time *t)
{
    uint64_t days = 0;
    for (int y = 1970; y < t->year; y++) {
        days += leap_year(y) ? 366 : 365;
    }
    for (int m = 1; m < t->month; m++) {
        days += (uint64_t)days_in_month(t->year, m);
    }
    days += (uint64_t)(t->day - 1);
    return days * 86400ull + (uint64_t)t->hour * 3600ull +
           (uint64_t)t->minute * 60ull + (uint64_t)t->second;
}

void time_init(void)
{
    struct rtc_time now;
    boot_epoch = rtc_read_time(&now) == 0 ? rtc_to_epoch(&now) : 0;
}

uint64_t time_now_seconds(void)
{
    struct rtc_time now;
    return rtc_read_time(&now) == 0 ? rtc_to_epoch(&now) : boot_epoch + pit_uptime_seconds();
}

uint64_t time_uptime_seconds(void)
{
    uint64_t pit = pit_uptime_seconds();
    if (!boot_epoch) {
        return pit;
    }
    uint64_t now = time_now_seconds();
    uint64_t rtc = now >= boot_epoch ? now - boot_epoch : 0;
    return rtc > pit ? rtc : pit;
}
