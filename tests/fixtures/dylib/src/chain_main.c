/* Executable linked with chained fixups: rebases (own data), binds to a dylib (data + function) and to libSystem. */
#include "darwin.h"
extern int chain_value;
extern int (*chain_fn)(int);
extern const char *chain_name;
extern int chain_get(void);
static const char *names[] = {"a", "bc", "def"};
static int local_double(int x) { return x + x; }
int (*local_fn)(int) = local_double;
int main(void) {
    puts(chain_name);                                /* bind to a data symbol, then libSystem */
    printf("%s %s\n", names[1], names[2]);           /* rebases */
    chain_value += 1;                                /* the executable writes the library's variable */
    if (local_fn(21) != 42) return 1;
    return chain_fn(chain_get() - 20);               /* 2 * (41 - 20) = 42 */
}
