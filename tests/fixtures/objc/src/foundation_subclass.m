/* Guest classes deriving from the native NSObject: -init/-dealloc/-description/-hash/-isEqual:/-copyWithZone: overrides, instances stored
   in native collections (retain/release balance, copied dictionary keys, %@ formatting), manual reference counting and NSAutoreleasePool.
   Exits with the number of failed checks; nothing may stay alive at exit. */
#include "Foundation/Foundation.h"

static int fails;
#define CHECK(...) do { if (!(__VA_ARGS__)) { fails++; NSLog(@"FAIL line %d: %s", __LINE__, #__VA_ARGS__); } } while (0)

static int live;       /* Point instances currently alive */
static int deallocs;

@interface Point : NSObject <NSCopying> {
    int x, y;
    NSString *label;
}
+ (Point *)pointWithX:(int)x y:(int)y;
- (Point *)initWithX:(int)x y:(int)y;
- (int)x;
- (int)y;
- (void)setLabel:(NSString *)l;
@end

@implementation Point
+ (Point *)pointWithX:(int)px y:(int)py { return [[[Point alloc] initWithX:px y:py] autorelease]; }
- (Point *)initWithX:(int)px y:(int)py {
    self = [super init];
    if (self) { x = px; y = py; live++; }
    return self;
}
- (void)dealloc { [label release]; live--; deallocs++; [super dealloc]; }
- (int)x { return x; }
- (int)y { return y; }
- (void)setLabel:(NSString *)l { NSString *old = label; label = [l copy]; [old release]; }
- (NSUInteger)hash { return (NSUInteger)(x * 31 + y); }
- (BOOL)isEqual:(id)other {
    if (![other isKindOfClass:[Point class]]) return NO;
    return x == [other x] && y == [other y];
}
- (NSString *)description { return [NSString stringWithFormat:@"(%d,%d)", x, y]; }
- (id)copyWithZone:(NSZone *)zone { return [[Point alloc] initWithX:x y:y]; }
@end

@interface Derived : Point
@end
@implementation Derived
- (NSString *)description { return [@"D" stringByAppendingString:[super description]]; }
@end

int main(void) {
    @autoreleasepool {
        Point *p = [[Point alloc] initWithX:1 y:2];
        CHECK([p retainCount] == 1 && live == 1);
        CHECK([[p description] isEqualToString:@"(1,2)"]);
        CHECK([p isKindOfClass:[NSObject class]] && [p isMemberOfClass:[Point class]] && [p respondsToSelector:@selector(hash)]);
        CHECK([Point isSubclassOfClass:[NSObject class]] && [[p class] superclass] == [NSObject class]);
        CHECK([p conformsToProtocol:@protocol(NSCopying)]);

        NSMutableArray *arr = [NSMutableArray array];
        [arr addObject:p];
        CHECK([p retainCount] == 2);                       /* the array retained it */
        [arr addObject:[Point pointWithX:3 y:4]];          /* autoreleased: the array keeps it alive */
        CHECK(live == 2);
        CHECK([[arr description] isEqualToString:@"(\n    (1,2),\n    (3,4)\n)"]);
        CHECK([arr containsObject:[Point pointWithX:3 y:4]] && [arr indexOfObject:[Point pointWithX:1 y:2]] == 0);   /* guest -isEqual: */
        [arr removeObjectAtIndex:1];                       /* last strong reference except the autorelease pool of this scope */

        Point *q = [p copy];                               /* +1 from copyWithZone: */
        CHECK(q != p && [q isEqual:p] && [q retainCount] == 1);

        NSMutableDictionary *d = [NSMutableDictionary dictionary];
        [d setObject:@"first" forKey:p];                   /* key is copied: p's count must not change... */
        CHECK([p retainCount] == 2);                       /* ...it is still 2 (array), the dictionary holds a copy */
        CHECK([[d objectForKey:q] isEqualToString:@"first"]);   /* found through guest -hash and -isEqual: */
        [d setObject:@"second" forKey:q];                  /* equal key: replaces the value, keeps one entry */
        CHECK([d count] == 1 && [[d objectForKey:p] isEqualToString:@"second"]);
        CHECK([[d description] isEqualToString:@"{\n    (1,2) = second;\n}"]);

        [p setLabel:@"origin"];
        Derived *dv = [[Derived alloc] initWithX:5 y:6];
        CHECK([[dv description] isEqualToString:@"D(5,6)"]);
        CHECK([dv isKindOfClass:[Point class]] && ![dv isMemberOfClass:[Point class]]);
        NSLog(@"p=%@ q=%@ dv=%@ arr=%@", p, q, dv, arr);
        [dv release];
        [q release];
        [p release];
        [arr removeAllObjects];
        [d removeAllObjects];
    }

    /* explicit NSAutoreleasePool */
    NSAutoreleasePool *pool = [[NSAutoreleasePool alloc] init];
    int before = live;
    for (int i = 0; i < 10; i++) (void)[Point pointWithX:i y:i];
    CHECK(live == before + 10);
    [pool drain];
    CHECK(live == before);

    CHECK(live == 0);
    CHECK(deallocs >= 15);
    NSLog(@"subclass checks done: %d failures, %d deallocs", fails, deallocs);
    return fails;
}
