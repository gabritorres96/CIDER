/* Fixture G: calls functions implemented by CIDER (strlen, abs, memset, memcpy). */
extern unsigned long strlen(const char *s);
extern int abs(int v);
extern void *memset(void *d, int c, unsigned long n);
extern void *memcpy(void *d, const void *s, unsigned long n);
int main(void) {
    char a[16], b[16];
    memset(a, 0x41, 15); a[15] = 0;
    memcpy(b, a, 16);
    return (int)strlen("CIDER runtime") + abs(-30) + (int)strlen(b) - 15;   /* 13 + 30 + 15 - 15 = 43 */
}
