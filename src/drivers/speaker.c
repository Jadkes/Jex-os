/**
 * @file speaker.c
 * @brief PC Speaker driver.
 *
 * The speaker is driven by PIT channel 2 gated through port 0x61.
 */

#include "speaker.h"
#include "ports.h"
#include "timer.h"

/*
 * play_sound - start a square wave on the speaker
 * @freq: tone frequency in Hertz
 */
void play_sound(uint32_t freq) {
    uint32_t divisor = PIT_BASE_FREQ_HZ / freq;
    uint8_t sysctl;

    outb(PIT_CMD, PIT_CMD_CH2 | PIT_CMD_ACCESS_LOHI | PIT_CMD_MODE_SQUARE);
    outb(PIT_CH2_DATA, (uint8_t)(divisor & 0xFF));
    outb(PIT_CH2_DATA, (uint8_t)(divisor >> 8));

    /* Gate ch2 and wire its output to the speaker */
    sysctl = inb(SYSCTL_PORT);
    if ((sysctl & (SYSCTL_TIMER2_GATE | SYSCTL_SPEAKER_EN)) !=
        (SYSCTL_TIMER2_GATE | SYSCTL_SPEAKER_EN)) {
        outb(SYSCTL_PORT, sysctl | SYSCTL_TIMER2_GATE | SYSCTL_SPEAKER_EN);
    }
}

/*
 * stop_sound - silence the speaker
 */
void stop_sound() {
    outb(SYSCTL_PORT, inb(SYSCTL_PORT) &
         (uint8_t)~(SYSCTL_TIMER2_GATE | SYSCTL_SPEAKER_EN));
}

/*
 * beep - play a tone for a fixed duration
 * @freq: tone frequency in Hertz
 * @ms:   duration in milliseconds
 */
void beep(uint32_t freq, uint32_t ms) {
    play_sound(freq);
    sleep(ms);
    stop_sound();
}
