int main() {
    // Print message using syscall (SYS_PRINT = 0)
    char* msg = "Hello from compiled C code on JexOS!\n";
    asm volatile("mov $0, %%eax" : : );
    asm volatile("mov %0, %%ebx" : : "r"(msg));
    asm volatile("int $0x80");
    
    return 42;
}