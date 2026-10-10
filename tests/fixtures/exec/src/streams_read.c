/* FILE streams, reading. The tests create data.bin (bytes 0..255) and lines.txt ("alpha\nbeta\n\ngamma" without a final newline).
   Return codes 10.. name the step that failed; 0 = everything behaved. */
#include "darwin.h"

int main(void) {
    FILE *f = fopen("data.bin", "rb");
    if (!f) return 10;
    unsigned char buf[300];
    if (fread(buf, 1, 16, f) != 16 || buf[0] != 0 || buf[15] != 15) return 11;
    if (ftell(f) != 16) return 12;
    if (fseek(f, 250, SEEK_SET) != 0) return 13;
    if (fgetc(f) != 250) return 14;
    if (ftell(f) != 251) return 15;
    if (ungetc(0xAB, f) != 0xAB) return 16;
    if (ftell(f) != 250) return 17;                              /* the pushed-back byte is part of the logical position */
    size_t n = fread(buf, 1, 10, f);                             /* 0xAB + 251..255 = 6 bytes, then end of file */
    if (n != 6 || buf[0] != 0xAB || buf[1] != 251 || buf[5] != 255) return 18;
    if (!feof(f) || ferror(f)) return 19;
    if (fgetc(f) != EOF) return 20;
    clearerr(f);
    if (feof(f)) return 21;
    if (fseek(f, -2, SEEK_END) != 0 || fgetc(f) != 254) return 22;
    if (fseek(f, -1, SEEK_CUR) != 0 || fgetc(f) != 254) return 23;  /* SEEK_CUR is relative to the logical position */
    if (fseek(f, 1000, SEEK_SET) != 0 || fread(buf, 1, 1, f) != 0 || !feof(f)) return 24;   /* seeking past the end is allowed, reading is not */
    rewind(f);
    if (feof(f) || fgetc(f) != 0) return 25;
    if (fread(buf, 1, 300, f) != 255 || !feof(f)) return 26;    /* a short read reports how many bytes it got and sets EOF */
    if (fwrite("x", 1, 1, f) != 0 || !ferror(f)) return 27;     /* writing to a read-only stream fails */
    if (fclose(f) != 0) return 28;
    if (fclose(f) != EOF || errno != EBADF) return 29;           /* second fclose */
    if (fread(buf, 1, 1, f) != 0 || errno != EBADF) return 30;   /* use after close */
    if (fgetc(f) != EOF || feof(f)) return 31;

    f = fopen("lines.txt", "r");
    if (!f) return 32;
    char line[8];
    if (!fgets(line, sizeof line, f) || strcmp(line, "alpha\n")) return 33;
    if (!fgets(line, sizeof line, f) || strcmp(line, "beta\n")) return 34;
    if (!fgets(line, sizeof line, f) || strcmp(line, "\n")) return 35;
    char tiny[4];
    if (!fgets(tiny, sizeof tiny, f) || strcmp(tiny, "gam")) return 36;     /* buffer smaller than the line: no overflow */
    if (!fgets(tiny, sizeof tiny, f) || strcmp(tiny, "ma")) return 37;
    if (fgets(tiny, sizeof tiny, f) || !feof(f)) return 38;                 /* EOF without a trailing newline */
    fclose(f);

    if (fopen("missing.bin", "r") || errno != ENOENT) return 39;
    if (fopen("../../outside.bin", "r") || errno != ENOENT) return 40;      /* ".." stops at the root */
    if (fopen("data.bin", "r+") == 0) return 41;                            /* update mode works */
    FILE *std = stdout;
    if (fileno(std) != 1 || fileno(stderr) != 2 || fileno(stdin) != 0) return 42;
    printf("streams read ok\n");
    return 0;
}
