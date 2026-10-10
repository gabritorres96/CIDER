/* libtls.dylib: its own thread-local storage, exported. */
__thread int lib_counter = 100;
__thread int lib_zero;
int lib_bump(void) { return ++lib_counter + lib_zero; }
