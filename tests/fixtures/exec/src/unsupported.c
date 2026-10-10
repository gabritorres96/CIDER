/* Fixture: printf conversions CIDER refuses. The guest ends with a runtime trap, never with guessed output. */
#include "darwin.h"

int main(int argc, char **argv) {
    int n = 0;
    char c = argc > 1 ? argv[1][0] : 'n';
    puts("before");
    if (c == 'n') printf("%n", &n);
    else if (c == 'a') printf("%a\n", 1.5);
    else if (c == 'L') printf("%Lf\n", (long double)1.5);
    else if (c == 'w') printf("%ls\n", (void *)0);
    else if (c == 'q') printf("%'d\n", 1000);
    puts("after");
    return 5;
}
