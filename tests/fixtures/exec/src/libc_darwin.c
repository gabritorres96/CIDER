/* Darwin libc functions that every Xcode-built program reaches: memchr, modf, fma, posix_memalign/malloc_size, pthread_main_np, __cxa_atexit,
   the stack-protector guard, ___chkstk_darwin and BSD getopt with its globals. Modes (argv[1]):
     ok            every check; exit code 42 and a line per atexit handler after main
     assert        __assert_rtn            abort        abort()
     getopt        getopt(argc, argv, "ab:c") over argv[2..]        getopt_colon      the same with ":ab:c" (silent, ':' for a missing argument) */
#include "darwin.h"

extern char *optarg;
extern int optind, optopt, opterr, optreset;
int getopt(int, char *const[], const char *);
void *memchr(const void *, int, size_t);
double modf(double, double *);
double fma(double, double, double);
int posix_memalign(void **, size_t, size_t);
size_t malloc_size(const void *);
void *malloc(size_t);
void *realloc(void *, size_t);
void free(void *);
int pthread_main_np(void);
int __cxa_atexit(void (*)(void *), void *, void *);
int atexit(void (*)(void));
void abort(void) __attribute__((noreturn));
void __assert_rtn(const char *, const char *, int, const char *) __attribute__((noreturn));
extern unsigned long __stack_chk_guard;

static char dso;
static void cxa_handler(void *arg) { printf("cxa(%s)\n", (const char *)arg); }
static void plain_handler(void) { puts("atexit"); }
static void *main_np_thread(void *unused) { (void)unused; return (void *)(long)pthread_main_np(); }

static int same(const char *a, const char *b) {
    while (*a && *a == *b) { ++a; ++b; }
    return *a == *b;
}

static int checks(void) {
    const char *s = "hello";
    if (memchr(s, 'l', 5) != s + 2) return 1;
    if (memchr(s, 'z', 5) != NULL) return 2;
    if (memchr(s, 'o', 4) != NULL) return 3;                      /* the 'o' is at index 4: outside the first 4 bytes */
    if (memchr(s, 'h', 0) != NULL) return 4;
    if (memchr(s, 0, 6) != s + 5) return 5;                       /* the terminator is searchable */
    double ip = 0;
    double f = modf(3.75, &ip);
    if (f != 0.75 || ip != 3.0) return 6;
    f = modf(-2.5, &ip);
    if (f != -0.5 || ip != -2.0) return 7;
    f = modf(5.0, &ip);
    if (f != 0.0 || ip != 5.0) return 8;
    if (fma(2.0, 3.0, 4.0) != 10.0) return 9;
    if (fma(0.1, 10.0, -1.0) == 0.0) return 10;                   /* fused: 0.1*10 is not exactly 1, a separate multiply-add would give 0 */
    /* posix_memalign: returns the error NUMBER and never touches errno */
    errno = 77;
    void *p = NULL;
    if (posix_memalign(&p, 64, 100) != 0 || !p || ((unsigned long)p & 63)) return 11;
    for (int i = 0; i < 100; ++i) ((char *)p)[i] = (char)i;
    if (malloc_size(p) < 100) return 12;
    p = realloc(p, 400);                                           /* moving out of an over-aligned block keeps the contents */
    if (!p) return 13;
    for (int i = 0; i < 100; ++i) if (((char *)p)[i] != (char)i) return 14;
    free(p);
    void *q = NULL;
    if (posix_memalign(&q, 4096, 10) != 0 || ((unsigned long)q & 4095)) return 15;
    free(q);
    void *keep = (void *)0x1234;
    if (posix_memalign(&keep, 3, 10) != EINVAL || keep != (void *)0x1234) return 16;        /* not a power of two: *memptr untouched */
    if (posix_memalign(&keep, 0, 10) != EINVAL) return 17;
    if (posix_memalign(&keep, 4, 10) != EINVAL) return 18;        /* smaller than sizeof(void *) */
    void *r = NULL;
    if (posix_memalign(&r, 8, 10) != 0 || !r) return 19;
    free(r);
    if (errno != 77) return 20;
    void *m = malloc(50);
    if (malloc_size(m) < 50) return 21;
    free(m);
    int local = 0;
    if (malloc_size(NULL) != 0 || malloc_size(&local) != 0) return 22;   /* not a malloc block: 0, never a trap */
    /* threads */
    if (pthread_main_np() != 1) return 23;
    pthread_t t;
    void *res = NULL;
    if (pthread_create(&t, NULL, main_np_thread, NULL) != 0 || pthread_join(t, &res) != 0) return 24;
    if (res != NULL) return 25;                                    /* pthread_main_np() is 0 on any other thread */
    /* the stack-protector guard is a stable, non-zero value whose lowest byte is zero */
    unsigned long g1 = __stack_chk_guard;
    if (g1 == 0 || (g1 & 0xff) != 0 || __stack_chk_guard != g1) return 26;
    /* ____chkstk_darwin: RAX in, RAX out, every other register preserved */
    unsigned long out[9];
    __asm__ volatile(
        "movl $8192, %%eax\n\t"
        "movq $0x1111, %%rcx\n\t movq $0x2222, %%rdx\n\t movq $0x3333, %%rsi\n\t movq $0x4444, %%rdi\n\t"
        "movq $0x5555, %%r8\n\t movq $0x6666, %%r9\n\t movq $0x7777, %%r10\n\t movq $0x8888, %%r11\n\t"
        "movq %0, %%rbx\n\t"
        "call ____chkstk_darwin\n\t"
        "movq %%rax, 0(%%rbx)\n\t movq %%rcx, 8(%%rbx)\n\t movq %%rdx, 16(%%rbx)\n\t movq %%rsi, 24(%%rbx)\n\t movq %%rdi, 32(%%rbx)\n\t"
        "movq %%r8, 40(%%rbx)\n\t movq %%r9, 48(%%rbx)\n\t movq %%r10, 56(%%rbx)\n\t movq %%r11, 64(%%rbx)\n\t"
        : : "r"(out) : "rax", "rbx", "rcx", "rdx", "rsi", "rdi", "r8", "r9", "r10", "r11", "memory", "cc");
    if (out[0] != 8192 || out[1] != 0x1111 || out[2] != 0x2222 || out[3] != 0x3333 || out[4] != 0x4444 ||
        out[5] != 0x5555 || out[6] != 0x6666 || out[7] != 0x7777 || out[8] != 0x8888) return 27;
    /* handlers run in reverse registration order, __cxa_atexit ones with their argument */
    if (__cxa_atexit(cxa_handler, "one", &dso) != 0) return 28;
    if (atexit(plain_handler) != 0) return 29;
    if (__cxa_atexit(cxa_handler, "three", &dso) != 0) return 30;
    return 0;
}

static int run_getopt(int argc, char **argv, const char *spec) {
    optind = 2;                                                    /* program and mode are not options: the guest WRITES the variable, libc reads it */
    int c;
    while ((c = getopt(argc, argv, spec)) != -1) {
        switch (c) {
            case 'a': puts("a"); break;
            case 'b': printf("b=%s\n", optarg); break;
            case 'c': puts("c"); break;
            case '?': printf("?(%c)\n", optopt); break;
            case ':': printf(":(%c)\n", optopt); break;
            default: printf("unexpected %d\n", c); break;
        }
    }
    printf("optind=%d", optind);
    if (optind < argc) printf(" rest=%s", argv[optind]);
    puts("");
    return 40 + (argc - optind);
}

int main(int argc, char **argv) {
    const char *mode = argc > 1 ? argv[1] : "ok";
    if (same(mode, "assert")) { int x = 2; if (x == 1) return 1; __assert_rtn("main", "libc_darwin.c", 777, "x == 1"); }
    if (same(mode, "abort")) { puts("before abort"); abort(); }
    if (same(mode, "getopt")) return run_getopt(argc, argv, "ab:c");
    if (same(mode, "getopt_colon")) return run_getopt(argc, argv, ":ab:c");
    int bad = checks();
    if (bad) { printf("check %d failed\n", bad); return bad; }
    puts("libc ok");
    return 42;
}
