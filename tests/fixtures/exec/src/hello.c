/* Fixture H: text output through puts and write. */
extern int puts(const char *s);
extern long write(int fd, const void *buf, unsigned long n);
int main(void) {
    puts("Hello from macOS!");
    write(1, "second line via write\n", 22);
    return 0;
}
