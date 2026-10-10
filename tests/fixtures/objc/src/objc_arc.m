/* Automatic reference counting (-fobjc-arc): the compiler inserts objc_retain/objc_release/objc_autorelease/objc_storeStrong,
   objc_retainAutoreleasedReturnValue, objc_initWeak/objc_loadWeak/objc_destroyWeak, objc_alloc/objc_alloc_init and the
   .cxx_destruct method that releases strong ivars and destroys weak ones. The counts below prove every one of them did its job. */
#include "objc_min.h"
#include "darwin.h"

static int live;

__attribute__((objc_root_class))
@interface Node {
    Class isa;
    Node *next;                         /* strong */
    __weak Node *back;                  /* weak: zeroed when the target dies */
    int value;
}
+ (id)alloc;
- (id)init;
- (Node *)makeChild:(int)v;
- (void)setNext:(Node *)n;
- (void)setBack:(Node *)n;
- (Node *)back;
- (Node *)next;
- (int)value;
- (void)setValue:(int)v;
- (void)dealloc;
@end

@implementation Node
+ (id)alloc { return class_createInstance(self, 0); }
- (id)init { ++live; return self; }
- (Node *)makeChild:(int)v { Node *n = [[Node alloc] init]; n->value = v; return n; }     /* returned +0: objc_autoreleaseReturnValue */
- (void)setNext:(Node *)n { next = n; }
- (void)setBack:(Node *)n { back = n; }
- (Node *)back { return back; }
- (Node *)next { return next; }
- (int)value { return value; }
- (void)setValue:(int)v { value = v; }
- (void)dealloc { --live; object_dispose(self); }      /* ARC adds the ivar cleanup before the user's code returns... via object_dispose -> .cxx_destruct */
@end

static __weak Node *global_weak;

int main(void) {
    @autoreleasepool {
        Node *a = [[Node alloc] init];                  /* objc_alloc_init */
        Node *b = [a makeChild:5];                      /* autoreleased return value, retained by the caller */
        if (live != 2) return 1;
        [a setNext:b];                                  /* strong ivar: retained */
        [b setBack:a];                                  /* weak ivar: not retained */
        if ([[b back] value] != 0 || [b back] != a) return 2;
        if ([[a next] value] != 5) return 3;
    }                                                   /* a is released at the end of the scope; it releases b through .cxx_destruct */
    if (live != 0) return 4;

    /* weak references are zeroed when the object deallocates */
    {
        Node *tmp = [[Node alloc] init];
        global_weak = tmp;
        if (global_weak != tmp || live != 1) return 5;
    }
    if (global_weak != nil || live != 0) return 6;

    /* a strong reference keeps the object alive across scopes; the weak one follows it */
    Node *keep = [[Node alloc] init];
    __weak Node *w = keep;
    {
        Node *other = keep;                             /* retain */
        (void)other;
    }
    if (w == nil || live != 1) return 7;
    keep = nil;                                         /* objc_storeStrong(nil): the last release */
    if (w != nil || live != 0) return 8;

    /* loop of temporaries drained by nested autorelease pools */
    for (int i = 0; i < 100; ++i) {
        @autoreleasepool {
            Node *t = [[Node alloc] init];
            Node *c = [t makeChild:i];
            [t setNext:c];
            if (live != 2) return 9;
        }
    }
    if (live != 0) return 10;
    printf("arc ok\n");
    return 42;
}
