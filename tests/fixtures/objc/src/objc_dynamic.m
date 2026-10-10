/* The runtime API: build a class at run time (objc_allocateClassPair / class_addIvar / class_addMethod / objc_registerClassPair),
   instantiate it, reach its ivar through the Ivar handle, call methods through objc_msgSend with explicit casts, swizzle. */
#include "objc_min.h"
#include "darwin.h"

__attribute__((objc_root_class))
@interface Seed {
    Class isa;
    int base;
}
+ (id)new;
- (int)base;
- (void)setBase:(int)v;
- (int)original;
@end
@implementation Seed
+ (id)new { return class_createInstance(self, 0); }
- (int)base { return base; }
- (void)setBase:(int)v { base = v; }
- (int)original { return 1; }
@end

static Ivar g_bias;
static int addBias(id self, SEL _cmd, int x) { return x + (int)(long)object_getIvar(self, g_bias); }
static int replacement(id self, SEL _cmd) { return 7; }

#define SEND_I_I(obj, sel, arg) ((int (*)(id, SEL, int))objc_msgSend)((obj), (sel), (arg))
#define SEND_I(obj, sel) ((int (*)(id, SEL))objc_msgSend)((obj), (sel))

int main(void) {
    Class seed = objc_getClass("Seed");
    if (!seed || objc_getClass("DoesNotExist") != Nil) return 1;
    Class sub = objc_allocateClassPair(seed, "Scientific", 0);
    if (!sub) return 2;
    if (!class_addIvar(sub, "bias", 4, 2, "i")) return 4;
    SEL addBiasSel = sel_registerName("addBias:");
    if (!class_addMethod(sub, addBiasSel, (IMP)addBias, "i@:i")) return 5;
    if (class_addMethod(sub, addBiasSel, (IMP)addBias, "i@:i")) return 6;                      /* already there: NO */
    objc_registerClassPair(sub);
    if (objc_allocateClassPair(seed, "Scientific", 0) != Nil) return 3;                       /* duplicate name refused once registered */
    if (class_addIvar(sub, "late", 4, 2, "i")) return 7;                                      /* too late after registration */
    if (objc_getClass("Scientific") != sub) return 8;
    g_bias = class_getInstanceVariable(sub, "bias");
    if (!g_bias || ivar_getOffset(g_bias) < 12 || class_getInstanceVariable(sub, "nope")) return 9;

    id obj = class_createInstance(sub, 0);
    if (!obj || object_getClass(obj) != sub) return 10;
    object_setIvar(obj, g_bias, (id)2);
    if (SEND_I_I(obj, addBiasSel, 40) != 42) return 11;                                       /* ivar (2) + 40 */
    ((void (*)(id, SEL, int))objc_msgSend)(obj, sel_registerName("setBase:"), 9);             /* inherited from Seed */
    if (SEND_I(obj, sel_registerName("base")) != 9) return 12;
    if (!class_respondsToSelector(sub, addBiasSel) || class_respondsToSelector(seed, addBiasSel)) return 13;

    Method m = class_getInstanceMethod(sub, sel_registerName("original"));                    /* inherited method */
    if (!m || method_getName(m) != sel_registerName("original")) return 14;
    IMP imp = method_getImplementation(m);
    if (((int (*)(id, SEL))imp)(obj, sel_registerName("original")) != 1) return 15;
    method_setImplementation(m, (IMP)replacement);                                            /* swizzle Seed's method */
    if (SEND_I(obj, sel_registerName("original")) != 7) return 16;
    /* nothing may remain from the instance when the class goes away */
    object_dispose(obj);
    objc_disposeClassPair(sub);
    if (objc_getClass("Scientific") != Nil) return 17;
    printf("dynamic classes ok\n");
    return 42;
}
