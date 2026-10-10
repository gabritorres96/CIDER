/* malloc/calloc/realloc/free provided by CIDER. Expected exit code: 4950 % 251 = 181. */
extern void *malloc(unsigned long n);
extern void *calloc(unsigned long a, unsigned long b);
extern void *realloc(void *p, unsigned long n);
extern void free(void *p);
int main(void) {
    unsigned char *p = malloc(100);
    for (int i = 0; i < 100; i++) p[i] = (unsigned char)i;
    p = realloc(p, 1000);
    int s = 0;
    for (int i = 0; i < 100; i++) s += p[i];            /* 4950 preserved by realloc */
    unsigned char *z = calloc(50, 4);
    for (int i = 0; i < 200; i++) s += z[i];            /* zero */
    free(p); free(z);
    return s % 251;                                      /* 181 */
}
