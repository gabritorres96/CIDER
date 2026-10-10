/* exit() called from a nested function: must stop the guest immediately, main never returns. */
extern void exit(int code);
extern int puts(const char *s);
__attribute__((noinline)) void leave(int c) { puts("leaving"); exit(c); }
int main(void) { leave(37); return 1; }
