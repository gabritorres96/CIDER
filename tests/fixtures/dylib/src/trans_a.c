/* libtransa.dylib depends on libtransb.dylib; the executable only knows libtransa. */
extern int trans_b_value(int);
int trans_a_value(int x) { return trans_b_value(x) + 2; }
