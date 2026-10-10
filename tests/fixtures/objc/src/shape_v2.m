/* libshape.dylib: the BASE class, in its newer version (one more ivar than the header the executable was compiled against). */
#include "objc_min.h"

__attribute__((objc_root_class))
@interface Shape {
    Class isa;
    int w, h;
    long extra;                                 /* added in version 2: the executable's Square was compiled without it */
}
+ (id)new;
- (int)area;
- (long)extra;
- (void)destroy;
@end
@implementation Shape
+ (id)new {
    Shape *s = class_createInstance(self, 0);
    s->w = 6;
    s->h = 7;
    s->extra = 99;
    return s;
}
- (int)area { return w * h; }
- (long)extra { return extra; }
- (void)destroy { object_dispose(self); }
@end
