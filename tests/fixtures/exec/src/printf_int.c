/* Fixture: printf with integer conversions, flags, widths, length modifiers and more arguments than registers. */
#include "darwin.h"

int main(void) {
    printf("Number: %d\n", 42);
    printf("[%5d][%-5d][%05d][%+d][% d]\n", 42, 42, 42, 42, 42);
    printf("[%x][%X][%#x][%o][%u]\n", 255, 255, 255, 8, 4000000000u);
    printf("[%ld][%lld][%hhd][%hd]\n", -5000000000L, 9000000000000000000LL, 300, 70000);
    printf("[%c][%3c][%-3c]%%\n", 'A', 'B', 'C');
    printf("[%*d][%-*d][%.*d]\n", 6, 7, 6, 7, 4, 7);
    printf("%d %d %d %d %d %d %d %d %d %d\n", 1, 2, 3, 4, 5, 6, 7, 8, 9, 10);   /* arguments 6..11 travel on the stack */
    return printf("end\n");                                                    /* printf returns the number of bytes: 4 */
}
