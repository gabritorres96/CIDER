/* Foundation misuse that must end the guest with an exception-style diagnosis (Apple raises NSException; ORVYN has no exceptions).
   argv[1]: 'a' index out of range, 'n' nil in addObject:, 'k' nil key, 'v' nil value, 's' substring out of range, 'u' unrecognized selector,
            'l' nil in an array literal, 'f' invalid NSLog format, 'c' character index out of range, 'r' remove on an empty array. */
#include "Foundation/Foundation.h"

int main(int argc, char **argv) {
    char mode = argc > 1 ? argv[1][0] : 'a';
    NSLog(@"before");
    NSMutableArray *array = [NSMutableArray arrayWithObjects:@1, @2, nil];
    NSMutableDictionary *dict = [NSMutableDictionary dictionary];
    id nothing = nil;
    switch (mode) {
        case 'a': return (int)[[array objectAtIndex:5] integerValue];
        case 'n': [array addObject:nothing]; break;
        case 'k': [dict setObject:@1 forKey:nothing]; break;
        case 'v': [dict setObject:nothing forKey:@"k"]; break;
        case 's': return (int)[[@"abc" substringToIndex:9] length];
        case 'u': return [(id)@"abc" performSelector:NSSelectorFromString(@"noSuchMethod")] != nil;
        case 'l': { NSArray *bad = @[@1, nothing]; return (int)[bad count]; }
        case 'f': NSLog(@"%n", (int *)0); break;
        case 'c': return (int)[@"abc" characterAtIndex:3];
        case 'r': [[NSMutableArray array] removeLastObject]; break;
    }
    NSLog(@"after");
    return 5;
}
