/**
 * @file shell.h
 * @brief Interactive Kernel Shell interface.
 */

#ifndef SHELL_H
#define SHELL_H

#include <stdint.h>

/**
 * @brief Initialize shell state (history, etc.).
 */
void shell_init();

/**
 * @brief Main entry point for the shell.
 * This function handles the main shell loop and input redirection.
 */
void shell_main();

/**
 * @brief Simple shell loop that re-prints the prompt and waits for input.
 */
void shell_loop();

/**
 * @brief Feed a single character of input into the shell.
 * Handles line editing and command execution.
 */
void shell_input(char key);

#endif // SHELL_H
