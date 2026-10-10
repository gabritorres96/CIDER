/* Single executable with chained fixups: rebases and binds only. */
#include "darwin.h"
static const char *words[] = {"alpha", "beta", "gamma"};
static int table[4] = {10, 20, 30, 40};
static int *pointers[] = {&table[3], &table[0], &table[2]};
int main(void) {
    int sum = 0;
    for (int i = 0; i < 3; ++i) sum += *pointers[i];                   /* 40 + 10 + 30 = 80 */
    printf("%s %s %d\n", words[0], words[2], sum);
    return sum + 5 - 43;                                               /* 42 */
}
