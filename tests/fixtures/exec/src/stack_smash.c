/* Built WITH -fstack-protector-all: the function overruns its buffer when asked to. Without an argument it behaves and returns 42; with
   one it smashes the canary and must end in ___stack_chk_fail, not in a corrupted return. */
#include "darwin.h"

__attribute__((noinline)) static int fill(char *src, int n) {
    char buf[16];
    for (int i = 0; i < n; ++i) ((volatile char *)buf)[i] = src[i];
    return ((volatile char *)buf)[3];
}

int main(int argc, char **argv) {
    (void)argv;
    char src[96];
    for (int i = 0; i < 96; ++i) src[i] = 'A';
    int r = fill(src, argc > 1 ? 96 : 8);
    puts("returned normally");
    return r == 'A' ? 42 : 1;
}
