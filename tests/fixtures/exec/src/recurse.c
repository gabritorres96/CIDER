/* Unbounded recursion: the guest stack (with its guard page) overflows. Expected: worker crash, CIDER survives. */
__attribute__((noinline)) static int f(int n) {
    volatile char pad[512];
    pad[0] = (char)n;
    return f(n + 1) + pad[0];
}
int main(void) { return f(0); }
