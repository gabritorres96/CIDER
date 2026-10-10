extern int cider_opt(void) __attribute__((weak_import));   /* libopt.dylib is a weak dependency that is not shipped */
int main(void) {
    if (&cider_opt) return cider_opt();
    return 7;
}
