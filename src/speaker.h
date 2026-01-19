#ifndef SPEAKER_H
#define SPEAKER_H

#include <stdint.h>

void play_sound(uint32_t nFrequence);
void stop_sound();
void beep(uint32_t freq, uint32_t ms);

#endif
