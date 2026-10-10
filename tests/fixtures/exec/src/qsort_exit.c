/* exit() called by a comparator in the middle of a sort: every nested host frame unwinds, atexit still runs. */
#include "darwin.h"

static void bye(void) { puts("bye"); }
static int cmp(const void *a, const void *b) {
    puts("comparing");
    exit(11);
    return 0;
}

int main(void) {
    int arr[4] = {3, 1, 2, 0};
    atexit(bye);
    qsort(arr, 4, sizeof arr[0], cmp);
    return 99;                            /* never reached */
}
