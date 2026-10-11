/* A Mach-O initializer that terminates the process with exit(7): main must never run. */
extern void exit(int);
__attribute__((constructor)) static void die(void) { exit(7); }
int orvyn_dummy_exit = 1;
