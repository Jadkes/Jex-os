/**
 * @file rtc.c
 * @brief Real-Time Clock (RTC) and CMOS driver.
 *
 * Provides access to the system clock for current date and time.
 */

#include "rtc.h"
#include "ports.h"

#define CMOS_ADDRESS 0x70
#define CMOS_DATA    0x71

/**
 * @brief Check if the RTC is currently performing an internal update.
 * Reading during an update can result in inconsistent data.
 * 
 * @return Non-zero if update is in progress.
 */
static int get_update_in_progress_flag() {
    outb(CMOS_ADDRESS, 0x0A);
    return (inb(CMOS_DATA) & 0x80);
}

/**
 * @brief Read a single byte from a CMOS register.
 * 
 * @param reg Register index to read.
 * @return Byte value.
 */
static unsigned char get_rtc_register(int reg) {
    outb(CMOS_ADDRESS, reg);
    return inb(CMOS_DATA);
}

/**
 * @brief Read the current wall-clock time from the RTC.
 * 
 * Automatically handles BCD (Binary Coded Decimal) to Binary conversion
 * and 12/24 hour format adjustments.
 * 
 * @return An rtc_time_t structure populated with current time.
 */
rtc_time_t read_rtc() {
    rtc_time_t time;
    unsigned char statusB;

    /* Ensure we don't read during a clock update */
    while (get_update_in_progress_flag());

    time.seconds = get_rtc_register(0x00);
    time.minutes = get_rtc_register(0x02);
    time.hours   = get_rtc_register(0x04);
    time.day     = get_rtc_register(0x07);
    time.month   = get_rtc_register(0x08);
    time.year    = get_rtc_register(0x09);
    
    /* Check Status Register B for data format (Binary vs BCD) */
    statusB = get_rtc_register(0x0B);

    /* Convert BCD to Binary if the hardware is in BCD mode (common) */
    if (!(statusB & 0x04)) {
        time.seconds = (time.seconds & 0x0F) + ((time.seconds / 16) * 10);
        time.minutes = (time.minutes & 0x0F) + ((time.minutes / 16) * 10);
        time.hours   = ( (time.hours & 0x0F) + (((time.hours & 0x70) / 16) * 10) ) | (time.hours & 0x80);
        time.day     = (time.day     & 0x0F) + ((time.day     / 16) * 10);
        time.month   = (time.month   & 0x0F) + ((time.month   / 16) * 10);
        time.year    = (time.year    & 0x0F) + ((time.year    / 16) * 10);
    }

    /* Convert 12 hour clock format to 24 hour if necessary */
    if (!(statusB & 0x02) && (time.hours & 0x80)) {
        time.hours = ((time.hours & 0x7F) + 12) % 24;
    }

    /* RTC provides 2-digit years. We assume 2000s for modern compatibility. */
    time.year += 2000;

    return time;
}
