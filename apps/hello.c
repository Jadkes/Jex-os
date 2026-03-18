/**
 * @file hello.c
 * @brief Standard "Hello World" application using JexOS LibC.
 *
 * Demonstrates basic console output using the printf implementation
 * in user-mode.
 */

#include <stdio.h>

/**
 * @brief Application entry point.
 */
int main() {
    printf("Hello, JexOS LibC!\n");
    printf("The year is %d.\n", 2026);
    printf("Hex test: 0x%x\n", 0xDEADBEEF);
    
    return 0;
}
