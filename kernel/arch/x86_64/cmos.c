#include <arch/io.h>
#include <tnu/time.h>

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
