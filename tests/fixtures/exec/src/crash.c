/* Guest fault: write to the null page. The runtime worker must die, CIDER must survive. */
int main(void) { *(volatile int *)0 = 1; return 0; }
