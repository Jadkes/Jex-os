#include "speaker.h"
#include "ports.h"
#include "timer.h"

/* Play sound using PIT Channel 2 */
void play_sound(uint32_t nFrequence) {
    uint32_t Div;
    uint8_t tmp;

    // Set the PIT to the desired frequency
    Div = 1193180 / nFrequence;
    outb(0x43, 0xB6);
    outb(0x42, (uint8_t) (Div) );
    outb(0x42, (uint8_t) (Div >> 8));

    // And play the sound using the speaker
    tmp = inb(0x61);
    if (tmp != (tmp | 3)) {
        outb(0x61, tmp | 3);
    }
}

/* Make it shut up */
void stop_sound() {
    uint8_t tmp = inb(0x61) & 0xFC;
    outb(0x61, tmp);
}

/* Make a beep for a given duration */
void beep(uint32_t freq, uint32_t ms) {
    play_sound(freq);
    sleep(ms);
    stop_sound();
}
