/* Same as initbase but linked with -init_offsets (__TEXT,__init_offsets, S_INIT_FUNC_OFFSETS). */
int orvyn_off_ready = 0;
__attribute__((constructor)) static void off_init(void) { orvyn_off_ready = 55; }
