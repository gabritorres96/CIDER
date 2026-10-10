/* Nested callbacks: the outer comparator itself calls qsort (with another guest comparator), so guest code runs inside a
   host function that runs inside guest code that runs inside a host function... */
#include "darwin.h"

static int depth_max;
static int cmp_inner(const void *a, const void *b) { return *(const int *)a - *(const int *)b; }

static int cmp_outer(const void *a, const void *b) {
    int tmp[4] = {4, 2, 3, 1};
    qsort(tmp, 4, sizeof tmp[0], cmp_inner);                     /* nested qsort inside a comparator */
    if (tmp[0] != 1 || tmp[3] != 4) depth_max = -1000;
    ++depth_max;
    return *(const int *)a - *(const int *)b;
}

int main(void) {
    int arr[6] = {6, 5, 4, 3, 2, 1};
    qsort(arr, 6, sizeof arr[0], cmp_outer);
    for (int i = 0; i < 6; ++i)
        if (arr[i] != i + 1) return 1;
    printf("outer comparisons: %d\n", depth_max);
    return depth_max > 0 ? 42 : 2;
}
