/**
 * @file timer.h
 * @brief Programmable Interval Timer (PIT) driver.
 *
 * Handles system ticks and provides timing/sleep functions.
 */

#ifndef TIMER_H
#define TIMER_H

#include <stdint.h>

/**
 * @brief Initialize the PIT at a specific frequency.
 * @param frequency Hertz (e.g., 100 for 10ms ticks).
 */
void init_timer(uint32_t frequency);

/**
 * @brief Block the current execution for a specified duration.
 * @param ms Milliseconds to sleep.
 */
void sleep(uint32_t ms);

/**
 * @brief Get the total number of system ticks since boot.
 */
uint32_t get_ticks();

#endif // TIMER_H
