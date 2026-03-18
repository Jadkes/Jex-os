/**
 * @file rtc.h
 * @brief Real-Time Clock (RTC) driver.
 *
 * Reads current date and time from the CMOS.
 */

#ifndef RTC_H
#define RTC_H

#include <stdint.h>

/**
 * @struct rtc_time_t
 * @brief Represents the current time and date.
 */
typedef struct {
    uint8_t seconds;
    uint8_t minutes;
    uint8_t hours;
    uint8_t day;
    uint8_t month;
    uint16_t year;
} rtc_time_t;

/**
 * @brief Read the current time and date from the RTC hardware.
 * @return A structure containing the current time.
 */
rtc_time_t read_rtc();

#endif // RTC_H
