/* Memory-management errors end the guest with a diagnosis. argv[1]: 'o' over-release, 'r' retain after free, 'p' wrong autorelease pool token,
   'n' autorelease with no pool (a warning, then a leak: the program continues), 'x' dealloc that forgets to free (the runtime frees it). */
#include "objc_min.h"
#include "darwin.h"

__attribute__((objc_root_class))
@interface Obj { Class isa; }
+ (id)alloc;
- (id)retain;
- (oneway void)release;
- (id)autorelease;
- (void)dealloc;
@end
static int freed;
@implementation Obj
+ (id)alloc { return class_createInstance(self, 0); }
- (id)retain { return objc_retain(self); }
- (oneway void)release { objc_release(self); }
- (id)autorelease { return objc_autorelease(self); }
- (void)dealloc { ++freed; object_dispose(self); }
@end

@interface Forgetful : Obj
- (void)dealloc;
@end
@implementation Forgetful
- (void)dealloc { ++freed; /* no [super dealloc]: the runtime must not leak the object */ }
@end

int main(int argc, char **argv) {
    char mode = argc > 1 ? argv[1][0] : 'o';
    puts("before");
    Obj *o = [Obj alloc];
    if (mode == 'o') { [o release]; [o release]; return 1; }                       /* second release: not a live object */
    if (mode == 'r') { [o release]; [o retain]; return 1; }                        /* retain after free */
    if (mode == 'p') { objc_autoreleasePoolPop((void *)0x1234); return 1; }        /* token that no pool ever returned */
    if (mode == 'n') { [o autorelease]; puts("continued"); return 5; }             /* no pool: warning + leak, execution goes on */
    if (mode == 'x') {
        Forgetful *f = [Forgetful alloc];
        [f release];
        printf("freed=%d\n", freed);
        return 6;
    }
    return 0;
}
