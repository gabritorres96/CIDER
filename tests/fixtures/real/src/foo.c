/* Helper dynamic library: install name @rpath/libfoo.dylib */
extern int getpid(void);
int foo_value(void) { return 1234 + getpid() * 0; }
