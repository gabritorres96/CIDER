/* Fixture I: LC_MAIN protocol main(argc, argv, envp, apple). */
extern int puts(const char *s);
int main(int argc, char **argv, char **envp, char **apple) {
    if (argc > 1) puts(argv[1]);
    int ok = argv[0] != 0 && argv[argc] == 0 && envp != 0 && apple != 0;
    return argc * 10 + ok;
}
