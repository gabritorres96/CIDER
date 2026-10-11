/* A program that CATCHES an exception: @try/@catch need the Darwin unwinder, which ORVYN does not provide. Built with Objective-C exceptions enabled it imports
   __objc_personality_v0 / objc_begin_catch / _Unwind_Resume; `orvyn run` must refuse it by naming them (exit 12), never run it with a broken catch. */
#include "Foundation/Foundation.h"

int main(void) {
    @try {
        [NSException raise:@"X" format:@"boom"];
    } @catch (NSException *e) {
        NSLog(@"caught %@", [e name]);
        return 1;
    }
    return 0;
}
