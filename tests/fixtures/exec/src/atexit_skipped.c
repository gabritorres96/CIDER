/* _exit() skips the atexit handlers. */
#include "darwin.h"

static void never(void) { puts("must not run"); }

int main(void) {
    atexit(never);
    puts("main");
    _exit(3);
}
