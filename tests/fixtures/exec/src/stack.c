/* Fixture C: locals, arrays on the stack, recursion. */
static int fib(int n) {
    volatile int pad[16];
    for (int i = 0; i < 16; i++) pad[i] = i + n;
    if (n < 2) return n;
    return fib(n - 1) + fib(n - 2) + (pad[3] - pad[3] - 3 + 3);
}
int main(void) {
    volatile unsigned char buf[200];
    for (int i = 0; i < 200; i++) buf[i] = (unsigned char)(i * 7);
    int s = 0;
    for (int i = 0; i < 200; i++) s += buf[i];
    return (fib(10) + (s & 0x3f)) & 0xff;
}
