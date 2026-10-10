/* Categories (methods added to a class, one overriding an existing method), +load of the class and of the category,
   and a protocol conformance query through the runtime. */
#include "objc_min.h"
#include "darwin.h"

@protocol Describable
- (int)describe;
@end

__attribute__((objc_root_class))
@interface Item <Describable> {
    Class isa;
}
+ (id)new;
- (int)value;
- (int)describe;
@end

static int order[8];
static int order_n;

@implementation Item
+ (void)load { order[order_n++] = 1; }
+ (id)new { return class_createInstance(self, 0); }
- (int)value { return 10; }
- (int)describe { return [self value] + 1; }
@end

@interface Item (Extras)
- (int)extra;
@end
@implementation Item (Extras)
+ (void)load { order[order_n++] = 2; }
- (int)extra { return 30; }
- (int)value { return 40; }                    /* the category's method wins over the class's */
@end

int main(void) {
    Item *item = [Item new];
    if (order_n != 2 || order[0] != 1 || order[1] != 2) return 1;      /* +load: class first, then its category, before main */
    if ([item extra] != 30) return 2;
    if ([item value] != 40) return 3;
    if ([item describe] != 41) return 4;                                /* describe calls the category's value */
    if (!class_conformsToProtocol(objc_getClass("Item"), objc_getProtocol("Describable"))) return 5;
    if (class_conformsToProtocol(objc_getClass("Item"), objc_getProtocol("Nope"))) return 6;
    printf("loads=%d\n", order_n);
    object_dispose((id)item);
    return [Item new] ? 42 : 7;
}
