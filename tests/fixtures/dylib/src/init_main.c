extern int cider_base_ready;
extern int cider_top_saw;
extern int cider_order[8];
extern int cider_order_n;
/* The executable has its own initializer: it runs last. */
__attribute__((constructor)) static void exe_init(void) { cider_order[cider_order_n++] = 3; }
int main(void) {
    if (cider_base_ready != 77) return 1;                 /* base initializer did not run */
    if (cider_top_saw != 77) return 2;                    /* top ran before base */
    if (cider_order_n != 3) return 3;                     /* an initializer ran twice or not at all */
    if (cider_order[0] != 1 || cider_order[1] != 2 || cider_order[2] != 3) return 4;
    return 42;
}
