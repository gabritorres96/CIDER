/* Fixture D: calls between functions (direct calls and an indirect call through a pointer in __DATA). */
__attribute__((noinline)) int add3(int a, int b, int c) { return a + b + c; }
__attribute__((noinline)) int twice(int v) { return v * 2; }
int (*g_fn)(int) = twice;   /* pointer in __DATA: needs a rebase */
int main(void) { return g_fn(add3(10, 20, 3)) + 1; }
