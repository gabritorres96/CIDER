/* Executable + two dylibs: (5+5)*4+2 = 42. */
extern int orvyn_add(int, int);
extern int orvyn_scale(int, int);
int main(void) { return orvyn_scale(orvyn_add(5, 5), 4) + 2; }
