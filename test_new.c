int main() {
    char* msg = "Hello from JexOS TCC!\n";
    asm volatile("mov $0, %%eax" : : );
    asm volatile("mov %0, %%ebx" : : "r"(msg));
    asm volatile("int $0x80");
    return 42;
}