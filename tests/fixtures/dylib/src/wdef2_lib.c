/* Compiled twice (-DVAL=7 -DGET=orvyn_a_get, -DVAL=9 -DGET=orvyn_b_get): two dylibs that both define the weak variable orvyn_shared. */
__attribute__((weak)) int orvyn_shared = VAL;
int GET(void) { return orvyn_shared; }
