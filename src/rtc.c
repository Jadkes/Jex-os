#include "rtc.h"
#include "ports.h"

#define CMOS_ADDRESS 0x70
#define CMOS_DATA    0x71

int get_update_in_progress_flag() {
    outb(CMOS_ADDRESS, 0x0A);
    return (inb(CMOS_DATA) & 0x80);
}

unsigned char get_rtc_register(int reg) {
    outb(CMOS_ADDRESS, reg);
    return inb(CMOS_DATA);
}

rtc_time_t read_rtc() {
    rtc_time_t time;
    unsigned char statusB;

    // Wait until RTC is not updating
    while (get_update_in_progress_flag());

    time.seconds = get_rtc_register(0x00);
    time.minutes = get_rtc_register(0x02);
    time.hours   = get_rtc_register(0x04);
    time.day     = get_rtc_register(0x07);
    time.month   = get_rtc_register(0x08);
    time.year    = get_rtc_register(0x09);
    
    // Check Status Register B to see if data is in Binary or BCD
    statusB = get_rtc_register(0x0B);

    // Convert BCD to Binary if necessary (Standard CMOS is usually BCD)
    if (!(statusB & 0x04)) {
        time.seconds = (time.seconds & 0x0F) + ((time.seconds / 16) * 10);
        time.minutes = (time.minutes & 0x0F) + ((time.minutes / 16) * 10);
        time.hours   = ( (time.hours & 0x0F) + (((time.hours & 0x70) / 16) * 10) ) | (time.hours & 0x80);
        time.day     = (time.day     & 0x0F) + ((time.day     / 16) * 10);
        time.month   = (time.month   & 0x0F) + ((time.month   / 16) * 10);
        time.year    = (time.year    & 0x0F) + ((time.year    / 16) * 10);
    }

    // Convert 12 hour clock to 24 hour clock if necessary
    if (!(statusB & 0x02) && (time.hours & 0x80)) {
        time.hours = ((time.hours & 0x7F) + 12) % 24;
    }

    // Calculate full year (RTC only gives last 2 digits, e.g., 23)
    // We assume we are in the 21st century (2000+)
    time.year += 2000;

    return time;
}
