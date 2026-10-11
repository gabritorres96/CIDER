/* Linked ONLY against Cocoa (an umbrella): the Foundation classes and NSLog bind to Cocoa's install name and must be found through its
   re-exports. The 42 comes from [NSNumber integerValue] on the two literals, as in foundation_hello. */
#include "Foundation/Foundation.h"

int main(void) {
    @autoreleasepool {
        NSArray *numbers = @[@20, @22];
        NSInteger total = 0;
        for (NSNumber *number in numbers)
            total += [number integerValue];
        NSLog(@"cocoa result: %ld", (long)total);
        return (int)total;
    }
}
