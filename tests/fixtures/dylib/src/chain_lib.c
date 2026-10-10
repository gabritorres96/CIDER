/* libchain.dylib linked with chained fixups: pointers inside its own data are chained rebases. */
int chain_value = 40;
static int twice(int x) { return x * 2; }
int (*chain_fn)(int) = twice;                      /* rebase to code */
const char *chain_name = "chain-lib";              /* rebase to a string */
int *chain_ptr = &chain_value;                     /* rebase to data */
int chain_get(void) { return *chain_ptr; }
