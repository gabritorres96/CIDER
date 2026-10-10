/* atexit handlers are guest functions: they run in reverse order after main returns. */
#include "darwin.h"

static void h1(void) { puts("handler 1"); }
static void h2(void) { puts("handler 2"); }
static void h3(void) { puts("handler 3"); atexit(h1); }       /* registered while exiting: runs next */

int main(void) {
    atexit(h1);
    atexit(h2);
    atexit(h3);
    puts("main");
    return 5;
}
