/* NSString behaviour checked from guest code. Exits with the number of failed checks (0 = all passed). */
#include "Foundation/Foundation.h"

static int fails;
#define CHECK(...) do { if (!(__VA_ARGS__)) { fails++; NSLog(@"FAIL line %d: %s", __LINE__, #__VA_ARGS__); } } while (0)

static int same(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

int main(void) {
    @autoreleasepool {
        NSString *a = @"hello";
        CHECK([a length] == 5);
        CHECK(same([a UTF8String], "hello"));
        CHECK([a characterAtIndex:1] == 'e');
        CHECK([a isEqualToString:@"hello"]);
        CHECK(![a isEqualToString:@"Hello"]);
        CHECK([a isEqual:[NSString stringWithUTF8String:"hello"]]);
        CHECK([a hash] == [[NSString stringWithString:a] hash]);
        CHECK([@"a" compare:@"b"] == NSOrderedAscending);
        CHECK([@"b" compare:@"a"] == NSOrderedDescending);
        CHECK([@"a" compare:@"a"] == NSOrderedSame);
        CHECK([a hasPrefix:@"he"] && ![a hasPrefix:@"lo"]);
        CHECK([a hasSuffix:@"lo"] && ![a hasSuffix:@"he"]);
        CHECK([a containsString:@"ell"] && ![a containsString:@"xyz"]);

        NSRange r = [a rangeOfString:@"ll"];
        CHECK(r.location == 2 && r.length == 2);
        r = [a rangeOfString:@"zz"];
        CHECK(r.location == NSNotFound);

        /* non-ASCII literals are UTF-16 in the binary; lengths count UTF-16 code units like Apple's NSString */
        NSString *u = @"héllo ✓ \U0001F600";
        CHECK([u length] == 10);
        CHECK(same([u UTF8String], "h\xc3\xa9llo \xe2\x9c\x93 \xf0\x9f\x98\x80"));
        CHECK([u characterAtIndex:1] == 0xe9);
        CHECK([[u substringToIndex:3] isEqualToString:@"hél"]);

        NSString *joined = [a stringByAppendingString:@", world"];
        CHECK([joined isEqualToString:@"hello, world"]);
        CHECK([[joined substringFromIndex:7] isEqualToString:@"world"]);
        CHECK([[joined substringWithRange:NSMakeRange(0, 5)] isEqualToString:@"hello"]);
        CHECK([[a uppercaseString] isEqualToString:@"HELLO"]);
        CHECK([[@"MiXed" lowercaseString] isEqualToString:@"mixed"]);

        NSString *f = [NSString stringWithFormat:@"%d-%s-%@-%.2f-%lu", 42, "cstr", @"obj", 3.14159, (unsigned long)7];
        CHECK([f isEqualToString:@"42-cstr-obj-3.14-7"]);
        NSString *g = [[NSString alloc] initWithFormat:@"[%5d|%-5d|%05d]", 42, 42, 42];
        CHECK([g isEqualToString:@"[   42|42   |00042]"]);
        CHECK([[NSString stringWithFormat:@"%@", nil] isEqualToString:@"(null)"]);

        CHECK([@"123" intValue] == 123 && [@"-45" integerValue] == -45);
        CHECK([@"2.5" doubleValue] == 2.5);
        CHECK([@"abc" intValue] == 0);

        NSArray *parts = [@"a,b,,c" componentsSeparatedByString:@","];
        CHECK([parts count] == 4);
        CHECK([[parts objectAtIndex:2] isEqualToString:@""]);
        CHECK([[parts componentsJoinedByString:@"+"] isEqualToString:@"a+b++c"]);

        NSString *c = [a copy];
        CHECK(c == a || [c isEqualToString:a]);
        CHECK([[NSString string] length] == 0);
        CHECK([NSString stringWithUTF8String:"\xff\xfe"] == nil);          /* invalid UTF-8 gives nil like Apple */
        CHECK([a isKindOfClass:[NSString class]] && [a isKindOfClass:[NSObject class]]);
        CHECK(same([NSStringFromClass([a class]) UTF8String], "NSConstantString"));
        CHECK([[NSString stringWithFormat:@"x"] class] == [NSString class]);

        NSLog(@"string %@ / %@ / %@", a, f, g);
        NSLog(@"checks done: %d failures", fails);
    }
    return fails;
}
