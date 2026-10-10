/* Fixture: open(argv[1]) read-only; exits with 0 on success (printing the number of bytes read, at most 15) or with the errno value. */
#include "darwin.h"

int main(int argc, char **argv) {
    if (argc < 2) return 99;
    int fd = open(argv[1], O_RDONLY);
    if (fd < 0) return errno;
    char b[16];
    ssize_t n = read(fd, b, 15);
    close(fd);
    printf("%ld\n", n);
    return 0;
}
