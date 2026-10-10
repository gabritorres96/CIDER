#include "objc_min.h"
__attribute__((objc_root_class))
@interface Calculator { Class isa; int total; }
+ (id)new;
- (int)add:(int)a to:(int)b;
- (int)total;
@end
@implementation Calculator
+ (id)new { return class_createInstance(self, 0); }
- (int)add:(int)a to:(int)b { total = a + b; return total; }
- (int)total { return total; }
@end
int main(void) {
    Calculator *c = [Calculator new];
    return [c add:20 to:22];
}
