/* Fixture: writing files. out.txt must end up containing "alpha\nbeta\ngamma\n"; escape attempts stay inside the root. */
#include "darwin.h"

int main(void) {
    int fd = open("out.txt", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 3) return 10;
    if (write(fd, "alpha\n", 6) != 6) return 11;
    if (write(fd, "beta\n", 5) != 5) return 12;
    if (close(fd) != 0) return 13;
    if (open("out.txt", O_WRONLY | O_CREAT | O_EXCL, 0644) != -1 || errno != EEXIST) return 14;   /* EEXIST is 17 on Darwin */
    fd = open("out.txt", O_WRONLY | O_APPEND);
    if (fd < 3) return 15;
    if (write(fd, "gamma\n", 6) != 6) return 16;
    close(fd);
    struct stat st;
    if (stat("out.txt", &st) != 0 || st.st_size != 17) return 17;
    if (access("out.txt", R_OK | W_OK) != 0) return 18;
    fd = open("out.txt", O_RDONLY);
    if (write(fd, "x", 1) != -1 || errno != EBADF) return 19;                                   /* read-only descriptor */
    close(fd);
    /* ".." can never leave the guest root: this creates <root>/escape.txt */
    fd = open("../../../escape.txt", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 3) return 20;
    write(fd, "inside", 6);
    close(fd);
    /* Windows-only syntax is refused, not interpreted */
    if (open("C:\\Windows\\win.ini", O_RDONLY) != -1 || errno != EINVAL) return 21;
    if (open("con", O_WRONLY | O_CREAT, 0644) != -1) return 22;
    if (open("out.txt", 0x40000000) != -1 || errno != EINVAL) return 23;                          /* unknown flag bit */
    printf("written\n");
    return 0;
}
