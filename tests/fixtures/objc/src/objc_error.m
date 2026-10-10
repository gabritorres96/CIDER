/* Runtime errors that must end the guest with a precise diagnosis instead of corrupting memory.
   argv[1]: 'u' unrecognized instance selector, 'c' unrecognized class selector, 'i' message to memory that is not an object,
            's' objc_msgSendSuper with a bogus class, 'd' use after dispose. */
#include "objc_min.h"
#include "darwin.h"

__attribute__((objc_root_class))
@interface Thing { Class isa; int v; }
+ (id)new;
- (int)known;
@end
@implementation Thing
+ (id)new { return class_createInstance(self, 0); }
- (int)known { return 3; }
@end

@interface Thing (Unknown)
- (int)notImplemented;
+ (int)alsoMissing;
@end

static long not_an_object[4] = {0x1234567, 0, 0, 0};

int main(int argc, char **argv) {
    char mode = argc > 1 ? argv[1][0] : 'u';
    Thing *t = [Thing new];
    puts("before");
    if (mode == 'u') return [t notImplemented];
    if (mode == 'c') return [Thing alsoMissing];
    if (mode == 'i') return [(Thing *)not_an_object known];
    if (mode == 'd') {
        object_dispose((id)t);
        return [t known];                       /* use after free: the freed memory no longer has a valid isa... or is simply gone */
    }
    puts("after");
    return 5;
}
