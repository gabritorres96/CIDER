__attribute__((constructor)) static void crash(void) { *(volatile int *)0 = 1; }
int cider_dummy_crash = 1;
