/* Fixture: process identity. */
#include "darwin.h"

int main(void) {
    printf("pid=%d ppid=%d\n", getpid(), getppid());
    return 0;
}
