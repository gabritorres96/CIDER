/* qsort with a comparator that is Mach-O code of this program: CIDER's qsort calls it through a host -> guest callback. */
#include "darwin.h"

static int calls;
static int compare(const void *a, const void *b) {
    int x = *(const int *)a;
    int y = *(const int *)b;
    ++calls;
    return (x > y) - (x < y);
}

int main(void) {
    int arr[10] = {9, 3, 7, 1, 8, 2, 6, 4, 5, 0};
    qsort(arr, 10, sizeof arr[0], compare);
    for (int i = 0; i < 10; ++i) printf("%d ", arr[i]);
    printf("\ncomparisons>0: %d\n", calls > 0);
    for (int i = 0; i < 10; ++i)
        if (arr[i] != i) return 1;
    return calls > 0 ? 42 : 2;           /* 42 only if the guest comparator really ran and the array is sorted */
}
