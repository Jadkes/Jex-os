#!/usr/bin/tcc
/**
 * @file hello_shebang.c
 * @brief Shebang execution demo.
 *
 * This file is treated as a script by the kernel. The #! line tells
 * the exec loader to use /usr/bin/tcc to compile and run this file
 * on the fly.
 */

int main() {
    char* msg = "Hello from shebang C execution!\n";
    
    /* Manual syscall 0 (SYS_PRINT) invocation using inline assembly */
    asm volatile("mov $0, %eax" : : );
    asm volatile("mov %0, %ebx" : : "r"(msg));
    asm volatile("int $0x80");
    
    return 0;
}
