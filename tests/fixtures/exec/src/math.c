/* Fixture B: integer math on volatile inputs (no constant folding). */
volatile long g_a = 123456789, g_b = 987, g_c = 31;
int main(void) {
    long x = g_a * g_b;
    x += g_c;
    x %= 251;
    long y = (g_a ^ g_b) & 0xff;
    unsigned long z = (unsigned long)g_a / (unsigned long)g_c;
    return (int)((x + y + (long)(z % 97)) & 0xff);
}
