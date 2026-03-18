/**
 * @file hello_tcc.c
 * @brief Self-hosting compilation demo.
 *
 * This file is intended to be compiled by the in-kernel 'cc' or 'tcc'
 * commands and then executed.
 */

int main() {
    /* Print message using raw syscall (SYS_PRINT = 0) */
    char* msg = "Hello from compiled C code on JexOS!\n";
    
    asm volatile("mov $0, %%eax" : : );
    asm volatile("mov %0, %%ebx" : : "r"(msg));
    asm volatile("int $0x80");
    
    return 42;
}
