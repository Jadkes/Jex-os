/**
 * @file speaker.c
 * @brief PC Speaker driver.
 *
 * Uses the PIT Channel 2 (connected to the speaker) to generate square waves
 * for audio output.
 */

#include "speaker.h"
#include "ports.h"
#include "timer.h"

/**
 * @brief Start playing a tone at the specified frequency.
 * 
 * @param nFrequence Target frequency in Hertz.
 */
void play_sound(uint32_t nFrequence) {
    uint32_t Div;
    uint8_t tmp;

    /* PIT base frequency is 1.193182 MHz */
    Div = 1193180 / nFrequence;
    
    /* Configure PIT Channel 2 */
    outb(0x43, 0xB6);
    outb(0x42, (uint8_t) (Div) );
    outb(0x42, (uint8_t) (Div >> 8));

    /* Enable the speaker by setting bits 0 and 1 of port 0x61 */
    tmp = inb(0x61);
    if (tmp != (tmp | 3)) {
        outb(0x61, tmp | 3);
    }
}

/**
 * @brief Stop the tone and silence the PC speaker.
 */
void stop_sound() {
    /* Disable speaker by clearing bits 0 and 1 of port 0x61 */
    uint8_t tmp = inb(0x61) & 0xFC;
    outb(0x61, tmp);
}

/**
 * @brief Play a tone for a specific duration.
 * 
 * @param freq Frequency in Hertz.
 * @param ms Duration in milliseconds.
 */
void beep(uint32_t freq, uint32_t ms) {
    play_sound(freq);
    sleep(ms);
    stop_sound();
}
