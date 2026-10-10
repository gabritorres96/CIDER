/* FILE streams, writing: binary data with every byte value (including 0x0A, 0x0D, 0x1A and 0x00, which a text-mode CRT would
   translate or truncate), formatted text, append mode, update mode ("w+") with rewind. The tests compare the files byte for byte. */
#include "darwin.h"

int main(void) {
    unsigned char all[256];
    for (int i = 0; i < 256; ++i) all[i] = (unsigned char)i;

    FILE *f = fopen("all.bin", "wb");
    if (!f) return 10;
    if (fwrite(all, 1, 256, f) != 256) return 11;
    if (ftell(f) != 256) return 12;
    if (fclose(f) != 0) return 13;

    f = fopen("text.txt", "w");
    if (!f) return 14;
    fputs("line1\n", f);
    fprintf(f, "line%d\r\n", 2);
    fputc('l', f);
    fwrite("ine3", 1, 4, f);
    fclose(f);

    f = fopen("text.txt", "a");
    if (!f) return 15;
    fputs("\nappended", f);
    fclose(f);

    f = fopen("update.bin", "w+");
    if (!f) return 16;
    fwrite("0123456789", 1, 10, f);
    rewind(f);
    char back[11] = {0};
    if (fread(back, 1, 10, f) != 10) return 17;
    if (memcmp(back, "0123456789", 10)) return 18;
    fseek(f, 3, SEEK_SET);
    fwrite("abc", 1, 3, f);                 /* overwrite in the middle after reading */
    fseek(f, 0, SEEK_END);
    if (ftell(f) != 10) return 19;
    fclose(f);

    f = fopen("all.bin", "wx");            /* exclusive create on an existing file must fail */
    if (f || errno != EEXIST) return 20;
    f = fopen("zz/none/x.bin", "w");       /* missing directory */
    if (f || errno != ENOENT) return 21;
    f = fopen("a.bin", "q");
    if (f || errno != EINVAL) return 22;
    printf("streams written\n");
    return 0;
}
