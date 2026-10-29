#ifndef MUOS_RTC_H
#define MUOS_RTC_H
#include "types.h"

typedef struct {
    uint8_t hour;
    uint8_t minute;
    uint8_t second;
} rtc_time_t;

/* Reads the CMOS real-time clock. Returns false if the RTC reports an
 * invalid value (or never leaves update-in-progress). */
bool_t rtc_read_time(rtc_time_t* out);

#endif
