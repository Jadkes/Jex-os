/**
 * @file power.h
 * @brief System Power Management (Reboot/Shutdown).
 */

#ifndef POWER_H
#define POWER_H

/**
 * @brief Perform a soft reboot of the system (using the 8042 keyboard controller).
 */
void reboot();

/**
 * @brief Shutdown the system (ACPI or APM based, if supported).
 */
void shutdown();

#endif // POWER_H
