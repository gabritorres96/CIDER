/* Executable using its own TLS and the dylib's TLS (through an imported thread-local descriptor). */
#include "darwin.h"
extern __thread int lib_counter;
extern int lib_bump(void);
__thread int exe_counter = 1;
int main(void) {
    exe_counter += 1;                       /* 2 */
    int a = lib_bump();                     /* 101 */
    lib_counter += 5;                       /* the executable writes the library's TLS variable: 106 */
    int b = lib_bump();                     /* 107 */
    if (a != 101 || b != 107) return 1;
    if (exe_counter != 2) return 2;         /* separate storage per image */
    return exe_counter + (lib_counter - 107) + 40;   /* 2 + 0 + 40 */
}
