/**
 * @file rtc.c
 * @brief Real-Time Clock (RTC) and CMOS driver.
 *
 * Provides access to the system clock for current date and time.
 */

#include "rtc.h"
#include "ports.h"

/* CMOS/RTC access: write register index to the address port, then
 * read/write the data port. */
#define CMOS_ADDRESS 0x70
#define CMOS_DATA    0x71

/* Register indices */
#define RTC_REG_SEC    0x00
#define RTC_REG_MIN    0x02
#define RTC_REG_HOUR   0x04
#define RTC_REG_DAY    0x07
#define RTC_REG_MONTH  0x08
#define RTC_REG_YEAR   0x09
#define RTC_REG_STATUS_A 0x0A
#define RTC_REG_STATUS_B 0x0B

/* Status A: update-in-progress flag */
#define RTC_UIP  0x80

/* Status B format bits */
#define RTC_FMT_24H  0x02
#define RTC_FMT_BIN  0x04

/* Hours bit 7 = PM in 12-hour mode */
#define RTC_HOUR_PM  0x80

/*
 * get_update_in_progress_flag - is the RTC mid-update?
 *
 * Reading time registers during an update returns inconsistent data.
 */
static int get_update_in_progress_flag() {
    outb(CMOS_ADDRESS, RTC_REG_STATUS_A);
    return (inb(CMOS_DATA) & RTC_UIP);
}

/*
 * get_rtc_register - read one CMOS register
 * @reg: register index
 */
static unsigned char get_rtc_register(int reg) {
    outb(CMOS_ADDRESS, reg);
    return inb(CMOS_DATA);
}

/*
 * read_rtc - read the wall clock, normalizing BCD and 12-hour formats
 *
 * Returns an rtc_time_t with binary fields and a full year (2000+).
 */
rtc_time_t read_rtc() {
    rtc_time_t time;
    unsigned char statusB;

    while (get_update_in_progress_flag());

    time.seconds = get_rtc_register(RTC_REG_SEC);
    time.minutes = get_rtc_register(RTC_REG_MIN);
    time.hours   = get_rtc_register(RTC_REG_HOUR);
    time.day     = get_rtc_register(RTC_REG_DAY);
    time.month   = get_rtc_register(RTC_REG_MONTH);
    time.year    = get_rtc_register(RTC_REG_YEAR);

    statusB = get_rtc_register(RTC_REG_STATUS_B);

    /* BCD → binary (the common hardware default) */
    if (!(statusB & RTC_FMT_BIN)) {
        time.seconds = (time.seconds & 0x0F) + ((time.seconds / 16) * 10);
        time.minutes = (time.minutes & 0x0F) + ((time.minutes / 16) * 10);
        time.hours   = ( (time.hours & 0x0F) + (((time.hours & 0x70) / 16) * 10) ) | (time.hours & RTC_HOUR_PM);
        time.day     = (time.day     & 0x0F) + ((time.day     / 16) * 10);
        time.month   = (time.month   & 0x0F) + ((time.month   / 16) * 10);
        time.year    = (time.year    & 0x0F) + ((time.year    / 16) * 10);
    }

    /* 12-hour → 24-hour */
    if (!(statusB & RTC_FMT_24H) && (time.hours & RTC_HOUR_PM)) {
        time.hours = ((time.hours & 0x7F) + 12) % 24;
    }

    /* RTC reports two-digit years; assume 2000s */
    time.year += 2000;

    return time;
}
