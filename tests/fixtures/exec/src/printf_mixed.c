/* Fixture: mixed integer/double/pointer arguments, stack arguments, and a guest-defined variadic function that
   forwards its va_list (built by the compiler: register save area + overflow area) to vsnprintf. */
#include "darwin.h"

static int format_to(char *buf, size_t n, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = vsnprintf(buf, n, fmt, ap);
    va_end(ap);
    return r;
}

static int log_line(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = vprintf(fmt, ap);
    va_end(ap);
    return r;
}

int main(void) {
    printf("%d %f %s %ld %f %c %u %f %p\n", 1, 1.5, "two", 3L, 4.25, '5', 6u, 7.75, (void *)0x1234);
    /* 6 integer registers are used up quickly; doubles and integers then go to the stack interleaved in call order */
    printf("%d|%s|%f|%d|%d|%d|%d|%f|%d|%f|%s|%lld\n", 1, "a", 1.5, 2, 3, 4, 5, 2.5, 6, 3.5, "z", 123456789012LL);
    char buf[256];
    int n = format_to(buf, sizeof buf, "%d %d %d %d %d %d %d %d | %f %f %f %f %f %f %f %f %f %f | %s",
                      1, 2, 3, 4, 5, 6, 7, 8, 0.5, 1.5, 2.5, 3.5, 4.5, 5.5, 6.5, 7.5, 8.5, 9.5, "tail");
    puts(buf);
    log_line("va_list printf: %d %.1f %s\n", 7, 2.5, "ok");
    return n;   /* 8 ints (15 chars incl. spaces) + ... computed by the test from the same format */
}
