/* CIDER real-world fixture: minimal executable. Freestanding: libSystem symbols are declared by hand. */
extern int puts(const char *s);
extern void exit(int code);

const char kGreeting[] = "hello from a real Mach-O";   /* __TEXT,__const / __cstring */
int g_initialized = 42;                                  /* __DATA,__data */
int g_zero[64];                                          /* __DATA,__bss (zerofill) */
static int local_counter = 7;

int cider_add(int a, int b) { return a + b + local_counter; }

int main(void) {
    g_zero[0] = cider_add(g_initialized, 1);
    puts(kGreeting);
    exit(g_zero[0] == 50 ? 0 : 1);
    return 0;
}
