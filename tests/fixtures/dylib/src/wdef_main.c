/* The executable also defines the weak symbols: dyld coalesces everything to the FIRST definition in load order (this one), including the
   library's own references. exit code = 22 (value) + 20 (function: 200 / 10) when coalescing works; 11 + 10 when it does not. */
extern int orvyn_lib_wval(void);
extern int orvyn_lib_wfn(void);
extern int *orvyn_lib_wptr(void);
__attribute__((weak)) int orvyn_wval = 22;
__attribute__((weak)) int orvyn_wfn(void) { return 200; }
int main(void) {
    if (orvyn_lib_wptr() != &orvyn_wval) return 1;     /* the library's address-of must be the executable's variable */
    return orvyn_lib_wval() + orvyn_lib_wfn() / 10;
}
