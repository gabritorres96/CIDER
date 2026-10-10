/* Executable depending on libSystem, @rpath/libfoo.dylib (load), @rpath/libweak.dylib (weak). */
extern int puts(const char *s);
extern int foo_value(void);
extern int weak_value(void) __attribute__((weak_import));

int main(void) {
    puts("app");
    int v = foo_value();
    if (weak_value) v += weak_value();
    return v == 1234 ? 0 : 1;
}
