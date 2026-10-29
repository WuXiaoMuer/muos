#ifndef MUOS_RTC_H
#define MUOS_RTC_H
#include "types.h"

typedef struct {
    uint8_t hour;
    uint8_t minute;
    uint8_t second;
} rtc_time_t;

typedef struct {
    uint16_t year;      /* full year, e.g. 2026 */
    uint8_t  month;     /* 1..12 */
    uint8_t  day;       /* 1..31 */
    uint8_t  hour;
    uint8_t  minute;
    uint8_t  second;
} rtc_datetime_t;

/* Reads the CMOS real-time clock. Returns false if the RTC reports an
 * invalid value (or never leaves update-in-progress). */
bool_t rtc_read_time(rtc_time_t* out);
bool_t rtc_read_datetime(rtc_datetime_t* out);

/* UTC seconds since 1970-01-01 (valid through 2106). */
uint32_t rtc_datetime_to_epoch(const rtc_datetime_t* dt);
uint32_t rtc_now(void);   /* 0 when the RTC read fails */

#endif
