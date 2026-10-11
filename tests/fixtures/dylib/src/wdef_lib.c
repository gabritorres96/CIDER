/* Weak definitions in a dylib (the library's own values are 11 and 100) and code that reaches them through the weak-binding table. */
__attribute__((weak)) int orvyn_wval = 11;
__attribute__((weak)) int orvyn_wfn(void) { return 100; }
int orvyn_lib_wval(void) { return orvyn_wval; }
int orvyn_lib_wfn(void) { return orvyn_wfn(); }
int *orvyn_lib_wptr(void) { return &orvyn_wval; }
