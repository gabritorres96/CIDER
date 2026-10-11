/* libinittop.dylib depends on libinitbase.dylib: its initializer must run AFTER the base one. */
extern int orvyn_base_ready;
extern int orvyn_order[8];
extern int orvyn_order_n;
int orvyn_top_saw = -1;
__attribute__((constructor)) static void top_init(void) {
    orvyn_top_saw = orvyn_base_ready;            /* 77 only if base_init already ran */
    orvyn_order[orvyn_order_n++] = 2;
}
