/* The reference Foundation program of ORVYN 0.4: literals, fast enumeration, NSLog and an autorelease pool, compiled with ARC.
   The 42 can only come from [NSNumber integerValue] being sent to the two NSNumber objects of the @[@20, @22] literal. */
#include "Foundation/Foundation.h"

int main(void) {
    @autoreleasepool {
        NSString *name = @"ORVYN";
        NSArray *numbers = @[@20, @22];
        NSInteger total = 0;
        for (NSNumber *number in numbers)
            total += [number integerValue];
        NSLog(@"%@ result: %ld", name, (long)total);
        return (int)total;
    }
}
