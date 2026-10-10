/* Executable + one dylib. The 42 is computed by cider_add inside libmath.dylib. */
extern int cider_add(int, int);
int main(void) { return cider_add(20, 22); }
