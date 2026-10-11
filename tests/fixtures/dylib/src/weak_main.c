extern int orvyn_opt(void) __attribute__((weak_import));   /* libopt.dylib is a weak dependency that is not shipped */
int main(void) {
    if (&orvyn_opt) return orvyn_opt();
    return 7;
}
