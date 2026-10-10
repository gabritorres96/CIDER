/* Blocks misuse that must end the guest with a diagnosis instead of corrupting memory.
   argv[1]: 'r' double release, 'g' release of a pointer that is not a block, 'c' copy of an unreadable pointer, 'n' copy/release of NULL (allowed). */
#include "Foundation/Foundation.h"
#include "Block.h"

typedef int (^IntBlock)(int);

int main(int argc, char **argv) {
    char mode = argc > 1 ? argv[1][0] : 'r';
    int k = 3;
    IntBlock b = Block_copy(^(int x) { return x + k; });
    NSLog(@"before");
    switch (mode) {
        case 'r': Block_release(b); Block_release(b); break;
        case 'g': { long fake[8] = {0}; Block_release((void *)fake); break; }
        case 'c': (void)_Block_copy((const void *)16); break;
        case 'n': (void)_Block_copy(0); _Block_release(0); Block_release(b); return 7;
    }
    NSLog(@"after");
    return 5;
}
