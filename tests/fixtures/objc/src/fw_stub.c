/* A recognised-but-unimplemented framework: ApplicationServices and Carbon functions are linked lazily and only called when an argument
   is given. Without arguments the program never touches them and must end with 42; with "ax" / "kbd" / "tis" the first call stops the run
   with the honest diagnostic of the stub (never a made-up result). */
extern unsigned char AXIsProcessTrusted(void);
extern unsigned char LMGetKbdType(void);
extern void *TISCopyCurrentKeyboardInputSource(void);

int main(int argc, char **argv) {
    if (argc > 1) {
        switch (argv[1][0]) {
            case 'a': return AXIsProcessTrusted() ? 1 : 2;
            case 'k': return LMGetKbdType();
            default: return TISCopyCurrentKeyboardInputSource() ? 3 : 4;
        }
    }
    return 42;
}
