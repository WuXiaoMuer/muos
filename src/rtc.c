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

bool_t rtc_read_time(rtc_time_t* out) {
    if (!out) return false;

    /* Wait for the update cycle to finish (usually immediate). */
    for (int i = 0; i < 1000; i++) {
        if (!rtc_updating()) break;
    }
    if (rtc_updating()) return false;

    uint8_t sec   = cmos_read(0x00);
    uint8_t min   = cmos_read(0x02);
    uint8_t hour  = cmos_read(0x04);
    uint8_t status = cmos_read(0x0B);

    /* If an update started while reading, discard this sample. */
    if (rtc_updating()) return false;

    /* The PM bit lives in bit7 of the raw hour byte and is not part of
     * the BCD digits, so capture it before conversion. */
    bool_t pm    = (hour & 0x80) != 0;
    bool_t h12   = !(status & 0x02);

    if (!(status & 0x04)) {   /* BCD mode */
        sec  = bcd_to_bin(sec);
        min  = bcd_to_bin(min);
        hour = bcd_to_bin((uint8_t)(hour & 0x7F));
    } else {
        hour = (uint8_t)(hour & 0x7F);
    }

    if (h12) {
        if (hour == 12) hour = 0;
        if (pm) hour += 12;
    }

    if (hour >= 24 || min >= 60 || sec >= 60) return false;

    out->hour = hour;
    out->minute = min;
    out->second = sec;
    return true;
}
