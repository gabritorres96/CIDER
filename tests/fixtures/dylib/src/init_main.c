extern int orvyn_base_ready;
extern int orvyn_top_saw;
extern int orvyn_order[8];
extern int orvyn_order_n;
/* The executable has its own initializer: it runs last. */
__attribute__((constructor)) static void exe_init(void) { orvyn_order[orvyn_order_n++] = 3; }
int main(void) {
    if (orvyn_base_ready != 77) return 1;                 /* base initializer did not run */
    if (orvyn_top_saw != 77) return 2;                    /* top ran before base */
    if (orvyn_order_n != 3) return 3;                     /* an initializer ran twice or not at all */
    if (orvyn_order[0] != 1 || orvyn_order[1] != 2 || orvyn_order[2] != 3) return 4;
    return 42;
}
