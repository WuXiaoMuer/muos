#include "rtc.h"
#include "io.h"

#define CMOS_ADDR 0x70
#define CMOS_DATA 0x71

/* NMI disable bit keeps the index/data pair consistent. */
static uint8_t cmos_read(uint8_t reg) {
    outb(CMOS_ADDR, reg | 0x80);
    return inb(CMOS_DATA);
}

static bool_t rtc_updating(void) {
    return (cmos_read(0x0A) & 0x80) != 0;
}

static uint8_t bcd_to_bin(uint8_t v) {
    return (uint8_t)((v & 0x0F) + (v >> 4) * 10);
}

static bool_t is_leap(int y) {
    return (y % 4 == 0 && y % 100 != 0) || (y % 400 == 0);
}

static uint8_t days_in_month(int y, int m) {
    static const uint8_t days[12] = { 31,28,31,30,31,30,31,31,30,31,30,31 };
    if (m < 1 || m > 12) return 0;
    if (m == 2 && is_leap(y)) return 29;
    return days[m - 1];
}

/* Days since 1970-01-01 (Howard Hinnant's civil-from-days inverse). */
static int32_t days_from_civil(int y, int m, int d) {
    y -= (m <= 2);
    int era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (unsigned)((153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1);
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return (int32_t)era * 146097 + (int32_t)doe - 719468;
}

uint32_t rtc_datetime_to_epoch(const rtc_datetime_t* dt) {
    if (!dt || dt->month < 1 || dt->month > 12 || dt->day < 1 ||
        dt->day > days_in_month(dt->year, dt->month) ||
        dt->hour > 23 || dt->minute > 59 || dt->second > 59) return 0;

    int32_t days = days_from_civil((int)dt->year, (int)dt->month, (int)dt->day);
    if (days < 0) return 0;
    return (uint32_t)days * 86400u + (uint32_t)dt->hour * 3600u +
           (uint32_t)dt->minute * 60u + dt->second;
}

bool_t rtc_read_datetime(rtc_datetime_t* out) {
    if (!out) return false;

    for (int i = 0; i < 1000; i++) {
        if (!rtc_updating()) break;
    }
    if (rtc_updating()) return false;

    uint8_t sec      = cmos_read(0x00);
    uint8_t min      = cmos_read(0x02);
    uint8_t hour     = cmos_read(0x04);
    uint8_t day      = cmos_read(0x07);
    uint8_t month    = cmos_read(0x08);
    uint8_t year     = cmos_read(0x09);
    uint8_t century  = cmos_read(0x32);
    uint8_t status   = cmos_read(0x0B);

    if (rtc_updating()) return false;

    bool_t pm  = (hour & 0x80) != 0;
    bool_t h12 = !(status & 0x02);

    if (!(status & 0x04)) {          /* BCD mode */
        sec     = bcd_to_bin(sec);
        min     = bcd_to_bin(min);
        hour    = bcd_to_bin((uint8_t)(hour & 0x7F));
        day     = bcd_to_bin(day);
        month   = bcd_to_bin(month);
        year    = bcd_to_bin(year);
        century = bcd_to_bin(century);
    } else {
        hour = (uint8_t)(hour & 0x7F);
    }

    if (h12) {
        if (hour == 12) hour = 0;
        if (pm) hour += 12;
    }

    if (month < 1 || month > 12 || day < 1 ||
        day > days_in_month(2000 + year, month) ||
        hour >= 24 || min >= 60 || sec >= 60) return false;

    uint16_t full_year;
    if (century >= 19 && century <= 21) {
        full_year = (uint16_t)(century * 100 + year);
    } else {
        full_year = (uint16_t)(2000 + year);
    }

    out->year = full_year;
    out->month = month;
    out->day = day;
    out->hour = hour;
    out->minute = min;
    out->second = sec;
    return true;
}

bool_t rtc_read_time(rtc_time_t* out) {
    rtc_datetime_t dt;
    if (!rtc_read_datetime(&dt)) return false;
    out->hour = dt.hour;
    out->minute = dt.minute;
    out->second = dt.second;
    return true;
}

uint32_t rtc_now(void) {
    rtc_datetime_t dt;
    if (!rtc_read_datetime(&dt)) return 0;
    return rtc_datetime_to_epoch(&dt);
}
