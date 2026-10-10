/* Invalid callback targets are refused (trap), never called: argv[1] = 'n' NULL, 'd' pointer into data, 'l' low address, 'h' pointer to the stack. */
#include "darwin.h"

static int table[4] = {4, 3, 2, 1};
int main(int argc, char **argv) {
    char c = argc > 1 ? argv[1][0] : 'n';
    int local = 0;
    void *fn = 0;
    if (c == 'd') fn = (void *)table;
    else if (c == 'l') fn = (void *)0x1000;
    else if (c == 'h') fn = (void *)&local;
    puts("before");
    qsort(table, 4, sizeof table[0], (int (*)(const void *, const void *))fn);
    puts("after");
    return 5;
}
