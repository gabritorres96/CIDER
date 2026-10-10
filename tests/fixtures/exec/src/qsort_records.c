/* qsort of strings (comparator calls strcmp, a libSystem thunk, from guest code), of 12-byte records (stability), and
   bsearch over the sorted result. */
#include "darwin.h"

struct rec { int key; int order; int pad; };

static int cmp_str(const void *a, const void *b) { return strcmp(*(const char *const *)a, *(const char *const *)b); }
static int cmp_rec(const void *a, const void *b) { return ((const struct rec *)a)->key - ((const struct rec *)b)->key; }
static int cmp_int(const void *a, const void *b) { return *(const int *)a - *(const int *)b; }

int main(void) {
    const char *words[] = {"pear", "apple", "fig", "banana", "cherry"};
    qsort(words, 5, sizeof words[0], cmp_str);
    for (int i = 0; i < 5; ++i) printf("%s ", words[i]);
    printf("\n");
    if (strcmp(words[0], "apple") || strcmp(words[4], "pear")) return 1;

    struct rec r[6] = {{3, 0, 0}, {1, 1, 0}, {3, 2, 0}, {2, 3, 0}, {1, 4, 0}, {3, 5, 0}};
    qsort(r, 6, sizeof r[0], cmp_rec);
    for (int i = 0; i < 6; ++i) printf("%d:%d ", r[i].key, r[i].order);
    printf("\n");
    if (r[0].order != 1 || r[1].order != 4 || r[3].order != 0 || r[4].order != 2 || r[5].order != 5) return 2;   /* stable */

    int sorted[8] = {1, 3, 5, 7, 9, 11, 13, 15};
    int key = 11, missing = 4;
    int *hit = bsearch(&key, sorted, 8, sizeof sorted[0], cmp_int);
    if (!hit || hit - sorted != 5) return 3;
    if (bsearch(&missing, sorted, 8, sizeof sorted[0], cmp_int)) return 4;
    return 42;
}
