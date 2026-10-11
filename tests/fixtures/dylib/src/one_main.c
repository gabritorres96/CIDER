/* Executable + one dylib. The 42 is computed by orvyn_add inside libmath.dylib. */
extern int orvyn_add(int, int);
int main(void) { return orvyn_add(20, 22); }
