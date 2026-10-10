/* Executable + two dylibs: (5+5)*4+2 = 42. */
extern int cider_add(int, int);
extern int cider_scale(int, int);
int main(void) { return cider_scale(cider_add(5, 5), 4) + 2; }
