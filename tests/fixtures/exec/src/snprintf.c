/* Fixture: snprintf truncation and return value, sprintf, and a zero-size probe. */
#include "darwin.h"

int main(void) {
    char b[8];
    int r = snprintf(b, sizeof b, "%s-%d", "abcdef", 12345);     /* would be "abcdef-12345": 12 bytes */
    printf("[%s] %d\n", b, r);
    r = snprintf(0, 0, "%d", 123456);                             /* length probe */
    printf("%d\n", r);
    char big[64];
    int w = sprintf(big, "%05d:%s:%.1f", 42, "x", 2.25);
    printf("%s %d\n", big, w);
    return r + w;                                                 /* 6 + 11 = 17 */
}
