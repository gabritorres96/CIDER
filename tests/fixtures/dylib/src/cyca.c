/* libcyca.dylib and libcycb.dylib depend on each other (legal in Mach-O). */
extern int cyc_b_double(int);
int cyc_a_leaf(int x) { return x + 1; }
int cyc_a_entry(int x) { return cyc_b_double(x); }
