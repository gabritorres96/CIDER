/* The reference program of ORVYN 0.4: a root class without Foundation. Everything below is compiled by clang to Apple-ABI
   Objective-C metadata + objc_msgSend calls; the 42 can only come from the method running inside the runtime's dispatch. */
#include "objc_min.h"

__attribute__((objc_root_class))
@interface Calculator {
    Class isa;
    int total;
}
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
    Calculator *calculator = [Calculator new];
    return [calculator add:20 to:22];
}
