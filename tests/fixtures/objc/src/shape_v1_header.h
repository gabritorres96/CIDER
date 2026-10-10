/* Version 1 of Shape, as the executable saw it when it was compiled: no `extra` ivar. */
#include "objc_min.h"
__attribute__((objc_root_class))
@interface Shape {
    Class isa;
    int w, h;
}
+ (id)new;
- (int)area;
- (long)extra;
- (void)destroy;
@end
