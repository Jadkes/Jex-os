/**
 * @file speaker.h
 * @brief PC Speaker driver.
 *
 * Controls the internal PC speaker for beeps and tones.
 */

#ifndef SPEAKER_H
#define SPEAKER_H

#include <stdint.h>

/**
 * @brief Start playing a sound at a specific frequency.
 * @param nFrequence Frequency in Hertz.
 */
void play_sound(uint32_t nFrequence);

/**
 * @brief Stop any sound currently playing.
 */
void stop_sound();

/**
 * @brief Play a beep sound for a specific duration.
 * @param freq Frequency in Hertz.
 * @param ms Duration in milliseconds.
 */
void beep(uint32_t freq, uint32_t ms);

#endif // SPEAKER_H
