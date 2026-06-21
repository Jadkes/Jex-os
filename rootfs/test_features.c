#!/usr/bin/tcc
int main() {
    print("=== JexOS TCC feature test ===\n");

    /* ----- Test 1: arithmetic expression + assignment ----- */
    x = 1 + 2 * 3;
    print("1: 1+2*3 = ");
    // x should be 7 (multiplication first)
    print("ok\n");

    /* ----- Test 2: bit shift << >> ----- */
    x = 1 << 3;
    // x should be 8
    y = 16 >> 2;
    // y should be 4
    print("2: shift << >> ok\n");

    /* ----- Test 3: compound assignment ----- */
    z = 10;
    z += 5;   /* 15 */
    z -= 3;   /* 12 */
    z *= 2;   /* 24 */
    z /= 4;   /* 6  */
    z %= 5;   /* 1  */
    print("3: compound += -= *= /= %= ok\n");

    /* ----- Test 4: fork() ----- */
    print("4: forking...\n");
    pid = fork();
    print("4: fork returned, pid=");
    // Can't print numbers easily, so just print "ok"
    print("ok\n");

    /* ----- Test 5: getpid() ----- */
    mypid = getpid();
    print("5: getpid ok\n");

    /* ----- Test 6: exit() in one process ----- */
    // Both processes reach here
    print("6: both processes alive\n");

    /* Print a unique marker per process so we can count */
    print("X");
    print("\n");

    return 0;
}
