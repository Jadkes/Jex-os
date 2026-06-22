/* Test: control flow (if/else, while, for) */
int main() {
    int x = 42;
    int y = 0;

    printf("Test: if/else\n");
    if (x > 10) {
        printf("x is big (42 > 10)\n");
        y = 100;
    }

    if (x > 100) {
        printf("x is huge - FAIL\n");
    } else {
        printf("x is normal - PASS (42 < 100)\n");
    }

    printf("y = %d\n", y);

    printf("Test: while loop\n");
    int i = 0;
    while (i < 5) {
        printf("while: %d\n", i);
        i = i + 1;
    }

    printf("Test: for loop\n");
    for (i = 0; i < 3; i = i + 1) {
        printf("for: %d\n", i);
    }

    printf("Done.\n");
    return 0;
}
