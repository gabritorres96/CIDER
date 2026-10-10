/* Thread-local storage in a single executable: initialised, zero-initialised, pointer-valued and static variables.
   Clang emits  movq _var@TLVP(%rip), %rdi ; callq *(%rdi)  -- no %gs access in the guest. */
#include "darwin.h"
__thread int counter = 5;
__thread int zeroed;
__thread const char *tname = "tls-name";
static __thread long local_tls = 7;
__thread char buffer[100];
__thread long big_zero[64];
static int bump(void) { return ++counter + zeroed + (int)local_tls; }
int main(void) {
    if (zeroed != 0 || big_zero[63] != 0 || buffer[99] != 0) return 1;       /* zero-fill */
    if (counter != 5 || local_tls != 7) return 2;                              /* initialised */
    int first = bump();                                                        /* 6 + 0 + 7 = 13 */
    zeroed = 20;
    local_tls += 2;
    buffer[0] = 'x'; buffer[99] = 'y';
    big_zero[63] = 3;
    puts(tname);                                                               /* pointer in the template is relocated */
    int second = bump();                                                       /* 7 + 20 + 9 = 36 */
    if (buffer[0] != 'x' || buffer[99] != 'y' || big_zero[63] != 3) return 3;
    return first + second - 7;                                                 /* 13 + 36 - 7 = 42 */
}
