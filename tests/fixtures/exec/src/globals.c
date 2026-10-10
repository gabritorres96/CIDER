/* Fixture E: initialised data, const data, pointers inside data (rebased). */
int g_table[4] = {10, 20, 30, 40};
int g_counter = 5;
const int *g_ptr = &g_table[1];
static const char *g_names[] = {"alpha", "beta", "gamma"};
const int g_const = 3;
int main(void) {
    g_counter += g_table[2];                 /* 35 */
    g_counter += *g_ptr;                     /* 55 */
    g_counter += g_names[1][0] - 97;         /* 56 ("beta"[0]=b) */
    return g_counter + g_const;              /* 59 */
}
