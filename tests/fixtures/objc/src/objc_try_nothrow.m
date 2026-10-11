/* A program that CONTAINS @try/@catch/@finally but only throws when asked to. Built with Objective-C exceptions enabled it imports the unwinding
   entry points (__objc_personality_v0, objc_begin_catch, _Unwind_Resume); ORVYN links them as honest stubs. Without an argument nothing is thrown, the
   normal path (including the @finally body) runs and the exit code is 42. With an argument the throw cannot be delivered to the @catch (no unwinder):
   the run must end with an explicit diagnostic, never with the catch "working". */
#include "Foundation/Foundation.h"

int main(int argc, char **argv) {
    (void)argv;
    @try {
        if (argc > 1) [NSException raise:@"ORVYNTest" format:@"requested"];
        NSLog(@"no throw");
    } @catch (NSException *e) {
        NSLog(@"caught %@", [e name]);
        return 1;
    } @finally {
        NSLog(@"finally");
    }
    return 42;
}
