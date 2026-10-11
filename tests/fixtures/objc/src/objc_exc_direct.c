/* Calls the exception-unwinding entry points DIRECTLY, as compiler-generated landing-pad code would after an unwind. ORVYN has no unwinder, so each call
   must stop the program with a diagnostic naming the function; nothing may return. Mode (argv[1]): b begin_catch, e end_catch, u _Unwind_Resume,
   r objc_exception_rethrow, p personality routine, n objc_exception_throw(nil). Without an argument it returns 42 (the symbols are imported, never called). */
extern void *objc_begin_catch(void *);
extern void objc_end_catch(void);
extern void _Unwind_Resume(void *);
extern void objc_exception_rethrow(void);
extern int __objc_personality_v0(int, int, unsigned long, void *, void *);
extern void objc_exception_throw(void *);
extern int puts(const char *);

int main(int argc, char **argv) {
    if (argc < 2) return 42;
    puts("before");
    switch (argv[1][0]) {
        case 'b': objc_begin_catch((void *)0); break;
        case 'e': objc_end_catch(); break;
        case 'u': _Unwind_Resume((void *)0); break;
        case 'r': objc_exception_rethrow(); break;
        case 'p': __objc_personality_v0(1, 1, 0, (void *)0, (void *)0); break;
        default: objc_exception_throw((void *)0); break;
    }
    puts("returned: this must never print");
    return 1;
}
