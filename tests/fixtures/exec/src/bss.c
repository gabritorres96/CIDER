/* Fixture F: zero-fill (__bss/__common) must be zero at start and writable. */
static int g_zero[1024];
static char g_buf[4096];
int g_common_zero[64];
int main(void) {
    int s = 0;
    for (int i = 0; i < 1024; i++) s += g_zero[i];
    for (int i = 0; i < 4096; i++) s += g_buf[i];
    for (int i = 0; i < 64; i++) s += g_common_zero[i];
    if (s != 0) return 100;
    for (int i = 0; i < 1024; i++) g_zero[i] = i;
    return g_zero[1000] & 0xff;              /* 1000 & 255 = 232 */
}
