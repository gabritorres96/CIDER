/* Plain absolute method lists as clang and ld64.lld emit them (neither can emit the relative format). tests/fixtures/relmeth.hpp rewrites the
   linked binary into the "small" method-list format ld64 produces for macOS 11+ targets. Instance and class methods, a category that
   overrides one method, a protocol. */
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
+ (int)answer;
- (int)value;
- (int)describe;
@end

@implementation Item
+ (id)new { return class_createInstance(self, 0); }
+ (int)answer { return 40; }
- (int)value { return 10; }
- (int)describe { return [self value] + 1; }
@end

@interface Item (Extras)
- (int)extra;
+ (int)bonus;
@end
@implementation Item (Extras)
- (int)extra { return 30; }
+ (int)bonus { return 2; }
- (int)value { return 40; }                    /* the category's method wins over the class's */
@end

int main(void) {
    Item *item = [Item new];
    if ([Item answer] != 40) return 1;
    if ([item extra] != 30) return 2;
    if ([item value] != 40) return 3;
    if ([item describe] != 41) return 4;
    if (!class_respondsToSelector(objc_getClass("Item"), sel_registerName("extra"))) return 5;
    printf("answer=%d\n", [Item answer] + [Item bonus]);
    object_dispose((id)item);
    return [Item answer] + [Item bonus];            /* 42 */
}
