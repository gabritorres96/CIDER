/* NSNumber, NSArray, NSMutableArray, NSDictionary and NSMutableDictionary checked from guest code (ARC). Exits with the number of failed checks. */
#include "Foundation/Foundation.h"

static int fails;
#define CHECK(...) do { if (!(__VA_ARGS__)) { fails++; NSLog(@"FAIL line %d: %s", __LINE__, #__VA_ARGS__); } } while (0)

int main(void) {
    @autoreleasepool {
        /* ---- NSNumber ---- */
        CHECK([@42 intValue] == 42 && [@42 integerValue] == 42 && [@42 longLongValue] == 42);
        CHECK([@3.5 doubleValue] == 3.5 && [@3.5 intValue] == 3);
        CHECK([@2.5f floatValue] == 2.5f);
        CHECK([@YES boolValue] && ![@NO boolValue] && [@7 boolValue]);
        CHECK([@-1 integerValue] == -1);
        CHECK([@1 isEqualToNumber:@1.0] && ![@1 isEqualToNumber:@2]);
        CHECK([@1 compare:@2] == NSOrderedAscending && [@2.5 compare:@2] == NSOrderedDescending && [@5 compare:@5] == NSOrderedSame);
        CHECK([[@42 stringValue] isEqualToString:@"42"] && [[@3.5 stringValue] isEqualToString:@"3.5"] && [[@YES stringValue] isEqualToString:@"1"]);
        CHECK([@42 hash] == [@42.0 hash]);
        CHECK([[@42 description] isEqualToString:@"42"]);
        CHECK([[NSNumber numberWithLongLong:0x123456789abLL] longLongValue] == 0x123456789abLL);
        CHECK([[NSNumber numberWithUnsignedChar:200] intValue] == 200);

        /* ---- NSArray ---- */
        NSArray *a = @[@1, @"two", @3];
        CHECK([a count] == 3);
        CHECK([[a objectAtIndex:0] intValue] == 1 && [[a objectAtIndex:1] isEqualToString:@"two"] && [a[2] intValue] == 3);
        CHECK([[a firstObject] intValue] == 1 && [[a lastObject] intValue] == 3);
        CHECK([a indexOfObject:@"two"] == 1 && [a indexOfObject:@"nope"] == NSNotFound);
        CHECK([a containsObject:@3] && ![a containsObject:@4]);
        CHECK([a isEqualToArray:@[@1, @"two", @3]] && ![a isEqualToArray:@[@1, @"two"]]);
        CHECK([a isEqual:[NSArray arrayWithObjects:@1, @"two", @3, nil]]);
        CHECK([[a arrayByAddingObject:@4] count] == 4 && [a count] == 3);
        CHECK([[a componentsJoinedByString:@"/"] isEqualToString:@"1/two/3"]);
        CHECK([[@[@1, @"two"] description] isEqualToString:@"(\n    1,\n    two\n)"]);
        CHECK([[@[] description] isEqualToString:@"(\n)"]);
        CHECK([[@[@[@1]] description] isEqualToString:@"(\n    (\n        1\n    )\n)"]);
        CHECK([[NSArray array] count] == 0 && [[NSArray arrayWithObject:@9] count] == 1);
        CHECK([[NSArray arrayWithArray:a] isEqualToArray:a]);
        CHECK([[a copy] isEqualToArray:a]);

        /* ---- NSMutableArray ---- */
        NSMutableArray *m = [NSMutableArray array];
        for (int i = 0; i < 100; i++) [m addObject:@(i)];
        CHECK([m count] == 100);
        int sum = 0, seen = 0;
        for (NSNumber *n in m) { sum += [n intValue]; seen++; }          /* 100 > the 16-slot buffer: several fast-enumeration batches */
        CHECK(sum == 4950 && seen == 100);
        [m removeObjectAtIndex:0];
        [m removeLastObject];
        CHECK([m count] == 98 && [m[0] intValue] == 1 && [[m lastObject] intValue] == 98);
        [m insertObject:@"head" atIndex:0];
        CHECK([m[0] isEqualToString:@"head"] && [m count] == 99);
        [m replaceObjectAtIndex:1 withObject:@"second"];
        m[2] = @"third";
        CHECK([m[1] isEqualToString:@"second"] && [m[2] isEqualToString:@"third"]);
        [m removeObject:@"second"];
        CHECK([m count] == 98 && [m[1] isEqualToString:@"third"]);
        NSMutableArray *snapshot = [m mutableCopy];
        NSArray *frozen = [m copy];
        [m removeAllObjects];
        CHECK([m count] == 0 && [snapshot count] == 98 && [frozen count] == 98);
        [snapshot addObject:@"x"];
        CHECK([snapshot count] == 99 && [frozen count] == 98);
        CHECK([[[NSMutableArray alloc] initWithCapacity:4] count] == 0);
        CHECK([[NSMutableArray arrayWithCapacity:4] count] == 0);

        /* ---- NSDictionary ---- */
        NSDictionary *d = @{@"a": @1, @"b": @"two", @"c": @[@3]};
        CHECK([d count] == 3);
        CHECK([[d objectForKey:@"a"] intValue] == 1 && [d[@"b"] isEqualToString:@"two"] && [d[@"c"] count] == 1);
        CHECK([d objectForKey:@"zzz"] == nil && d[@"zzz"] == nil);
        CHECK([[d allKeys] count] == 3 && [[d allValues] count] == 3);
        CHECK([d isEqualToDictionary:@{@"c": @[@3], @"b": @"two", @"a": @1}]);
        CHECK(![d isEqualToDictionary:@{@"a": @1}]);
        CHECK([[@{@"k": @1, @"j": @"v"} description] isEqualToString:@"{\n    k = 1;\n    j = v;\n}"]);
        CHECK([[@{} description] isEqualToString:@"{\n}"]);
        int keyCount = 0;
        for (NSString *key in d) { CHECK(d[key] != nil); keyCount++; }
        CHECK(keyCount == 3);
        CHECK([[NSDictionary dictionaryWithObject:@5 forKey:@"five"][@"five"] intValue] == 5);
        CHECK([[NSDictionary dictionaryWithDictionary:d] isEqualToDictionary:d]);

        /* ---- NSMutableDictionary ---- */
        NSMutableDictionary *md = [NSMutableDictionary dictionary];
        [md setObject:@1 forKey:@"one"];
        md[@"two"] = @2;
        md[@"one"] = @11;
        CHECK([md count] == 2 && [md[@"one"] intValue] == 11);
        [md removeObjectForKey:@"two"];
        CHECK([md count] == 1 && md[@"two"] == nil);
        md[@"one"] = nil;                                              /* assigning nil through the subscript removes */
        CHECK([md count] == 0);
        for (int i = 0; i < 50; i++) md[[NSString stringWithFormat:@"key%d", i]] = @(i * i);
        CHECK([md count] == 50 && [md[@"key7"] intValue] == 49);
        NSDictionary *imm = [md copy];
        [md removeAllObjects];
        CHECK([md count] == 0 && [imm count] == 50);
        NSMutableDictionary *mm = [imm mutableCopy];
        mm[@"extra"] = @0;
        CHECK([mm count] == 51 && [imm count] == 50);

        NSLog(@"collections: %@ %@", a, d[@"b"]);
        NSLog(@"checks done: %d failures", fails);
    }
    return fails;
}
