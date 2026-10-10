/* Asks for 1.5 GiB. Under the supervisor's job memory limit malloc fails and main returns 3 (limit enforced);
   without a limit it would return 4. */
extern void *malloc(unsigned long n);
extern void *memset(void *d, int c, unsigned long n);
int main(void) {
    unsigned long n = 1536ul * 1024 * 1024;
    char *p = malloc(n);
    if (!p) return 3;
    for (unsigned long i = 0; i < n; i += 4096) p[i] = 1;
    return 4;
}
