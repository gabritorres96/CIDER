/* libinitbase.dylib: a Mach-O initializer (__mod_init_func) changes exported globals before main. */
int orvyn_base_ready = 0;
int orvyn_order[8];
int orvyn_order_n = 0;
__attribute__((constructor)) static void base_init(void) {
    orvyn_base_ready = 77;
    orvyn_order[orvyn_order_n++] = 1;
}
