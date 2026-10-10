/* exit() inside an atexit handler replaces the exit code and stops the handler sequence. */
#include "darwin.h"

static void first(void) { puts("first"); }
static void quitter(void) { puts("quitter"); exit(9); }

int main(void) {
    atexit(first);
    atexit(quitter);
    exit(3);
}
