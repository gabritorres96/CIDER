/* Blocks ABI from guest code (MRC): global/stack/heap blocks, captures, __block variables, captured Objective-C objects, copy/dispose helpers,
   blocks invoked by the runtime (enumeration) and nested callbacks. Exits with the number of failed checks. */
#include "Foundation/Foundation.h"
#include "Block.h"

static int fails;
#define CHECK(...) do { if (!(__VA_ARGS__)) { fails++; NSLog(@"FAIL line %d: %s", __LINE__, #__VA_ARGS__); } } while (0)

typedef int (^IntBlock)(int);
typedef void (^VoidBlock)(void);

static int deallocs;
@interface Token : NSObject
@end
@implementation Token
- (void)dealloc { deallocs++; [super dealloc]; }
@end

__attribute__((noinline)) static int call_int(IntBlock b, int x) { return b(x); }
__attribute__((noinline)) static void call_void(VoidBlock b) { b(); }
__attribute__((noinline)) static VoidBlock make_counter(int *out) {
    __block int count = 0;
    VoidBlock b = ^{ count++; *out = count; };
    return Block_copy(b);                         /* the __block variable must outlive this frame */
}
static IntBlock global_block;                      /* no captures: a global block */

int main(void) {
    NSAutoreleasePool *pool = [[NSAutoreleasePool alloc] init];

    /* 1. global block */
    global_block = ^(int x) { return x * 2; };
    CHECK(call_int(global_block, 21) == 42);
    IntBlock g2 = Block_copy(global_block);
    CHECK(g2 == global_block);                      /* copying a global block returns it */
    Block_release(g2);

    /* 2. stack block capturing by value; copy snapshots the value */
    int k = 5;
    IntBlock stackb = ^(int x) { return x + k; };
    IntBlock heapb = Block_copy(stackb);
    CHECK(heapb != stackb);
    k = 100;                                        /* captured BY VALUE: unchanged */
    CHECK(call_int(heapb, 1) == 6);
    IntBlock again = Block_copy(heapb);             /* heap block: same object, one more reference */
    CHECK(again == heapb);
    Block_release(again);
    Block_release(heapb);

    /* 3. __block variable shared between the block and the frame */
    __block int shared = 10;
    VoidBlock inc = ^{ shared += 5; };
    VoidBlock inc_heap = Block_copy(inc);
    call_void(inc_heap);
    call_void(inc_heap);
    CHECK(shared == 20);                            /* the frame sees the updates made through the heap copy */
    shared = 1;
    call_void(inc_heap);
    CHECK(shared == 6);
    Block_release(inc_heap);

    int seen = 0;
    VoidBlock counter = make_counter(&seen);
    call_void(counter); call_void(counter); call_void(counter);
    CHECK(seen == 3);
    Block_release(counter);

    /* 4. captured Objective-C object: retained by the copy helper, released by the dispose helper */
    Token *t = [[Token alloc] init];
    CHECK([t retainCount] == 1);
    VoidBlock holder = ^{ (void)t; };
    VoidBlock holder_heap = Block_copy(holder);
    CHECK([t retainCount] == 2);
    [t release];
    CHECK(deallocs == 0);
    Block_release(holder_heap);                     /* last reference: dispose helper releases the token */
    CHECK(deallocs == 1);

    /* 5. a block that captures a block */
    IntBlock inner = Block_copy(^(int x) { return x + 1; });
    IntBlock outer = Block_copy(^(int x) { return inner(inner(x)); });
    Block_release(inner);                           /* outer still owns inner */
    CHECK(call_int(outer, 40) == 42);
    Block_release(outer);

    /* 6. blocks invoked by the runtime: enumeration with a nested enumeration inside the block */
    NSArray *outer_array = @[@1, @2, @3];
    NSArray *inner_array = @[@10, @20];
    __block int total = 0, visits = 0;
    [outer_array enumerateObjectsUsingBlock:^(id obj, NSUInteger idx, BOOL *stop) {
        [inner_array enumerateObjectsUsingBlock:^(id o2, NSUInteger i2, BOOL *s2) {
            total += [obj intValue] * [o2 intValue];
            visits++;
        }];
    }];
    CHECK(total == 180 && visits == 6);             /* (1+2+3) * (10+20) */

    __block int stop_seen = 0;
    [outer_array enumerateObjectsUsingBlock:^(id obj, NSUInteger idx, BOOL *stop) {
        stop_seen++;
        if (idx == 1) *stop = YES;
    }];
    CHECK(stop_seen == 2);                          /* *stop ends the enumeration */
    CHECK([outer_array indexOfObjectPassingTest:^BOOL(id obj, NSUInteger idx, BOOL *stop) { return [obj intValue] == 3; }] == 2);
    CHECK([outer_array indexOfObjectPassingTest:^BOOL(id obj, NSUInteger idx, BOOL *stop) { return NO; }] == NSNotFound);

    __block int dsum = 0;
    [@{@"a": @1, @"b": @2} enumerateKeysAndObjectsUsingBlock:^(id key, id obj, BOOL *stop) { dsum += [obj intValue]; }];
    CHECK(dsum == 3);

    /* 7. a heap block stored in a collection is retained by it (blocks are objects) */
    int base = 7;
    IntBlock stored = Block_copy(^(int x) { return x + base; });
    NSMutableArray *list = [NSMutableArray array];
    [list addObject:stored];
    Block_release(stored);
    IntBlock fetched = [list objectAtIndex:0];
    CHECK(call_int(fetched, 1) == 8);
    [list removeAllObjects];

    [pool drain];
    CHECK(deallocs == 1);
    NSLog(@"blocks checks done: %d failures", fails);
    return fails;
}
