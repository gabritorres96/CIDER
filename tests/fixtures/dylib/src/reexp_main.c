extern int core_fn(void);              /* defined in libcore, requested from libfacade (re-export) */
int main(void) { return core_fn(); }
