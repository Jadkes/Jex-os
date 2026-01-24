#include "../user/include/stdio.h"
#include "../user/include/unistd.h"
#include "../user/include/stdlib.h"

int main() {
    printf("Testing unistd.h functions:\n");
    
    // Test write
    write(STDOUT_FILENO, "Testing write() system call\n", 28);
    
    // Test getpid
    int pid = getpid();
    printf("Process ID: %d\n", pid);
    
    // Test isatty
    if (isatty(STDOUT_FILENO)) {
        printf("STDOUT is a terminal\n");
    } else {
        printf("STDOUT is not a terminal\n");
    }
    
    // Test sbrk
    void* old_brk = sbrk(0);
    printf("Old program break: 0x%x\n", (unsigned int)old_brk);
    
    void* new_brk = sbrk(1024);
    printf("After sbrk(1024): 0x%x\n", (unsigned int)new_brk);
    
    if (new_brk != (void*)-1) {
        printf("sbrk() succeeded - allocated 1024 bytes\n");
        char* mem = (char*)old_brk;
        mem[0] = 'H'; mem[1] = 'e'; mem[2] = 'l'; mem[3] = 'l'; mem[4] = 'o';
        mem[5] = '\0';
        printf("Written to allocated memory: %s\n", mem);
    } else {
        printf("sbrk() failed\n");
    }
    
    printf("unistd tests completed!\n");
    return 0;
}