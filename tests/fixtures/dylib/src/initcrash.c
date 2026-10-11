__attribute__((constructor)) static void crash(void) { *(volatile int *)0 = 1; }
int orvyn_dummy_crash = 1;
