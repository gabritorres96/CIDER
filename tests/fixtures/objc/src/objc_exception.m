/* NSException creation and an UNCAUGHT throw (no @try here). argv[1]: r = [NSException raise:format:], t = [ex raise] on an exception object,
   n = no throw (exception objects only). A throw ends the guest like an uncaught exception does on macOS. */
#include "Foundation/Foundation.h"

int main(int argc, char **argv) {
    char mode = argc > 1 ? argv[1][0] : 'n';
    NSAutoreleasePool *pool = [[NSAutoreleasePool alloc] init];
    NSException *ex = [NSException exceptionWithName:@"ORVYNError" reason:@"made by hand" userInfo:nil];
    int ok = [[ex name] isEqualToString:@"ORVYNError"] && [[ex reason] isEqualToString:@"made by hand"] && [ex userInfo] == nil;
    NSLog(@"before throw, objects ok=%d", ok);
    if (mode == 'r') [NSException raise:@"ORVYNRaise" format:@"value %d and %@", 42, @"text"];
    if (mode == 't') [ex raise];
    [pool drain];
    return ok ? 0 : 1;
}
