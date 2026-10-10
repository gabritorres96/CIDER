/* The stream pool is finite: fopen fails with EMFILE (24) when it is exhausted, and slots are reusable after fclose.
   Prints the number of streams opened. Forged FILE pointers never crash the runtime. */
#include "darwin.h"

int main(void) {
    FILE *files[100];
    int opened = 0;
    for (int i = 0; i < 100; ++i) {
        files[i] = fopen("limit.tmp", "w");
        if (!files[i]) break;
        ++opened;
    }
    int err = errno;
    printf("opened=%d errno=%d\n", opened, err);
    for (int i = 0; i < opened; ++i) fclose(files[i]);
    FILE *again = fopen("limit.tmp", "r");               /* slots are free again */
    if (!again) return 10;
    fclose(again);
    /* forged / invalid FILE pointers: refused with EBADF, never dereferenced */
    FILE *forged[] = {(FILE *)0, (FILE *)1, (FILE *)0x12345678, (FILE *)&opened, (FILE *)((char *)stdout + 1), (FILE *)((char *)stdout + 64 * 100)};
    for (int i = 0; i < 6; ++i) {
        char b[4];
        if (fread(b, 1, 1, forged[i]) != 0) return 20 + i;
        if (fputc('x', forged[i]) != EOF) return 30 + i;
        if (fclose(forged[i]) != EOF) return 40 + i;
        if (errno != EBADF) return 50 + i;
    }
    return err == 24 ? 42 : 1;
}
