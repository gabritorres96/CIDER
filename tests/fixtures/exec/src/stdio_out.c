/* Fixture: every output function, interleaving stdout and stderr; the exact byte streams are asserted by the tests. */
#include "darwin.h"

int main(void) {
    puts("out-1");
    fputs("err-1\n", stderr);
    putchar('A');
    putchar('\n');
    fprintf(stderr, "err-%d\n", 2);
    fwrite("fw", 1, 2, stdout);
    fwrite("\n", 1, 1, stdout);
    write(1, "w1\n", 3);
    write(2, "w2\n", 3);
    fputc('Z', stdout);
    putc('\n', stdout);
    printf("end %s\n", "ok");
    fflush(stdout);
    fflush(0);
    return 3;
}
