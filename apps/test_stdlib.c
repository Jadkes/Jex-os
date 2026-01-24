#include "../user/include/stdio.h"
#include "../user/include/stdlib.h"
#include "../user/include/string.h"

int main() {
    printf("Testing stdlib.h functions:\n");
    
    // Test malloc and free
    printf("Testing malloc/free...\n");
    char* ptr = (char*)malloc(100);
    if (ptr) {
        strcpy(ptr, "Hello from malloc!");
        printf("malloc'd string: %s\n", ptr);
        free(ptr);
        printf("Memory freed successfully\n");
    } else {
        printf("malloc failed!\n");
    }
    
    // Test atoi
    printf("Testing atoi...\n");
    char* num_str = "42";
    int num = atoi(num_str);
    printf("atoi('%s') = %d\n", num_str, num);
    
    printf("stdlib tests completed!\n");
    return 0;
}