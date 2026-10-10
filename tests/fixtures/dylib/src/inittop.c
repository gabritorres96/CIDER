/* libinittop.dylib depends on libinitbase.dylib: its initializer must run AFTER the base one. */
extern int cider_base_ready;
extern int cider_order[8];
extern int cider_order_n;
int cider_top_saw = -1;
__attribute__((constructor)) static void top_init(void) {
    cider_top_saw = cider_base_ready;            /* 77 only if base_init already ran */
    cider_order[cider_order_n++] = 2;
}
