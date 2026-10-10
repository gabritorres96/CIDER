/* Fixture: aggregate returns (rax / rax:rdx) and signatures that mix an integer and a double. */
#include "darwin.h"

int main(void) {
    div_t d = div(47, 5);                    /* {9, 2} in rax */
    ldiv_t l = ldiv(-100000000000L, 7);      /* {-14285714285, -5} in rax:rdx */
    double x = ldexp(3.0, 4);                /* xmm0 + edi: a positional thunk would misplace the int */
    int e = 0;
    double m = frexp(48.0, &e);              /* 0.75 * 2^6 */
    double f = fmod(10.5, 3.0);
    printf("%d %d %ld %ld %.1f %.2f %d %.1f\n", d.quot, d.rem, l.quot, l.rem, x, m, e, f);
    return d.quot + d.rem + e;               /* 9 + 2 + 6 = 17 */
}
