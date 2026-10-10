/* libfacade re-exports libcore through LC_REEXPORT_DYLIB; it defines nothing that main uses. */
int facade_unused(void) { return 0; }
