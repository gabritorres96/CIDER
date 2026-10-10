/* libinitbase.dylib: a Mach-O initializer (__mod_init_func) changes exported globals before main. */
int cider_base_ready = 0;
int cider_order[8];
int cider_order_n = 0;
__attribute__((constructor)) static void base_init(void) {
    cider_base_ready = 77;
    cider_order[cider_order_n++] = 1;
}
