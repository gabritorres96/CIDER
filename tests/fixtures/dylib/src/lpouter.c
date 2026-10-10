/* installed as @executable_path/libs/liblpouter.dylib; its dependency is @loader_path/inner/liblpinner.dylib */
extern int lp_inner(void);
int lp_outer(void) { return lp_inner() + 2; }
