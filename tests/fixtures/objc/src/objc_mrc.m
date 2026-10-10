/* Manual reference counting on a root class: retain/release/autorelease implemented with the runtime's entry points,
   dealloc releasing ivars, nested autorelease pools, inheritance (dealloc chain) and object counting. */
#include "objc_min.h"
#include "darwin.h"

static int live;
static int order[16];
static int order_n;

__attribute__((objc_root_class))
@interface Obj {
    Class isa;
    Obj *child;
    int tag;
}
+ (id)alloc;
+ (id)objWithTag:(int)t;
- (id)initWithTag:(int)t;
- (id)retain;
- (oneway void)release;
- (id)autorelease;
- (void)setChild:(Obj *)c;
- (int)tag;
- (void)dealloc;
@end

@implementation Obj
+ (id)alloc { return class_createInstance(self, 0); }
+ (id)objWithTag:(int)t { return [[[self alloc] initWithTag:t] autorelease]; }
- (id)initWithTag:(int)t { tag = t; ++live; return self; }
- (id)retain { return objc_retain(self); }
- (oneway void)release { objc_release(self); }
- (id)autorelease { return objc_autorelease(self); }
- (void)setChild:(Obj *)c { [c retain]; [child release]; child = c; }
- (int)tag { return tag; }
- (void)dealloc {
    order[order_n++] = tag;
    [child release];
    --live;
    object_dispose(self);
}
@end

@interface Special : Obj { int extra; }
- (void)dealloc;
@end
@implementation Special
- (void)dealloc { order[order_n++] = -[self tag]; [super dealloc]; }
@end

int main(void) {
    /* 1. create, retain, release */
    Obj *a = [[Obj alloc] initWithTag:1];
    if (live != 1) return 1;
    [a retain];
    [a release];
    if (live != 1) return 2;                         /* still one reference */
    [a release];
    if (live != 0 || order_n != 1 || order[0] != 1) return 3;

    /* 2. autorelease pools, nested */
    void *outer = objc_autoreleasePoolPush();
    Obj *x = [Obj objWithTag:10];
    {
        void *inner = objc_autoreleasePoolPush();
        Obj *y = [Obj objWithTag:20];
        (void)y;
        if (live != 2) return 4;
        objc_autoreleasePoolPop(inner);               /* y goes, x stays */
        if (live != 1) return 5;
    }
    if ([x tag] != 10) return 6;
    objc_autoreleasePoolPop(outer);
    if (live != 0) return 7;

    /* 3. ownership graph: parent -> child, released in dealloc order parent first */
    order_n = 0;
    Obj *parent = [[Obj alloc] initWithTag:100];
    Obj *child = [[Obj alloc] initWithTag:200];
    [parent setChild:child];
    [child release];                                  /* the parent keeps it alive */
    if (live != 2) return 8;
    [parent release];
    if (live != 0 || order_n != 2 || order[0] != 100 || order[1] != 200) return 9;

    /* 4. subclass: the dealloc chain runs subclass first */
    order_n = 0;
    Special *s = [[Special alloc] initWithTag:7];
    [s release];
    if (live != 0 || order_n != 2 || order[0] != -7 || order[1] != 7) return 10;

    /* 5. retain count survives many retains */
    Obj *many = [[Obj alloc] initWithTag:3];
    for (int i = 0; i < 1000; ++i) [many retain];
    for (int i = 0; i < 1000; ++i) [many release];
    if (live != 1) return 11;
    [many release];
    if (live != 0) return 12;
    printf("mrc ok\n");
    return 42;
}
