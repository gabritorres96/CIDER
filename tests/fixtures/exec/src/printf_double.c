/* Fixture: printf with doubles (they travel in xmm registers; AL tells the callee how many). */
#include "darwin.h"

int main(void) {
    printf("Float: %.2f\n", 3.14);
    printf("[%f][%e][%g][%G]\n", 0.5, 12345.6789, 0.0001234, 1e-10);
    printf("[%10.3f][%-10.1f][%+.0f][%08.2f]\n", 3.14159, 2.5, 2.5, -3.14159);
    double inf = __builtin_inf(), nan = __builtin_nan("");
    printf("[%f][%f][%5.1f][%F][%+f]\n", inf, -inf, nan, inf, inf);
    /* ten doubles: eight in xmm0-7, two on the stack */
    printf("%g %g %g %g %g %g %g %g %g %g\n", 1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0, 9.0, 10.5);
    return 0;
}
