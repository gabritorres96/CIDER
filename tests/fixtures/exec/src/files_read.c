/* Fixture: reading files through open/fstat/lseek/read/close. The tests create input.txt ("hello, world!\n") under the
   guest root. Return codes 10.. identify the step that failed; 0 means everything matched. */
#include "darwin.h"

int main(void) {
    char buf[64];
    int fd = open("input.txt", O_RDONLY);
    if (fd < 0) return 10;
    if (fd < 3) return 20;                                   /* descriptors 0-2 are the console */
    struct stat st;
    if (fstat(fd, &st) != 0) return 11;
    if ((st.st_mode & S_IFMT) != S_IFREG) return 12;
    off_t end = lseek(fd, 0, SEEK_END);
    if (end != st.st_size) return 13;
    if (lseek(fd, 7, SEEK_SET) != 7) return 14;
    ssize_t n = read(fd, buf, 5);                            /* "world" */
    if (n != 5) return 15;
    buf[5] = 0;
    printf("size=%lld word=%s\n", st.st_size, buf);
    if (lseek(fd, -1, SEEK_CUR) != 11) return 16;
    n = read(fd, buf, 64);                                   /* "d!\n" */
    if (n != 3) return 17;
    n = read(fd, buf, 64);                                   /* end of file */
    if (n != 0) return 18;
    if (close(fd) != 0) return 19;
    if (close(fd) != -1 || errno != EBADF) return 21;        /* second close: EBADF (9 on Darwin) */
    if (open("missing.txt", O_RDONLY) != -1 || errno != ENOENT) return 22;
    struct stat s2;
    if (stat("input.txt", &s2) != 0 || s2.st_size != 14) return 23;
    if (stat("no-such-file", &s2) != -1 || errno != ENOENT) return 24;
    if (access("input.txt", R_OK) != 0) return 25;
    if (access("no-such-file", F_OK) != -1) return 26;
    char cwd[16];
    if (!getcwd(cwd, sizeof cwd) || cwd[0] != '/' || cwd[1] != 0) return 27;
    return 0;
}
