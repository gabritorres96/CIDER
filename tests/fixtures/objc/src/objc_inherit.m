/* Inheritance: instance and class methods, [super ...] in both (objc_msgSendSuper2), ivars of the subclass after the superclass's,
   overriding, and class methods reaching the root class's instance methods through the metaclass chain. */
#include "objc_min.h"
#include "darwin.h"

__attribute__((objc_root_class))
@interface Base {
    Class isa;
    int a;
}
+ (id)new;
+ (int)kind;
- (int)describe;
- (int)a;
- (void)setA:(int)v;
- (void)destroy;
@end

@interface Derived : Base {
    int b;
}
+ (int)kind;
- (int)describe;
- (void)setB:(int)v;
- (int)sum;
@end

@implementation Base
+ (id)new { return class_createInstance(self, 0); }
+ (int)kind { return 1; }
- (int)describe { return 100 + a; }
- (int)a { return a; }
- (void)setA:(int)v { a = v; }
- (void)destroy { object_dispose(self); }
@end

@implementation Derived
+ (int)kind { return 2 + [super kind]; }                 /* 3 */
- (int)describe { return [super describe] + b; }         /* 100 + a + b */
- (void)setB:(int)v { b = v; }
- (int)sum { return [self a] + b; }
@end

int main(void) {
    Derived *d = [Derived new];
    [d setA:10];
    [d setB:7];
    if ([d describe] != 117) return 1;
    if ([Derived kind] != 3 || [Base kind] != 1) return 2;
    Base *base = [Base new];
    [base setA:4];
    if ([base describe] != 104) return 3;
    /* the instance sizes follow the non-fragile layout: Base = isa + int (16 with padding), Derived adds an int */
    if (class_getInstanceSize(objc_getClass("Base")) != 16 || class_getInstanceSize(objc_getClass("Derived")) != 16) return 4;
    if (class_getSuperclass(objc_getClass("Derived")) != objc_getClass("Base")) return 5;
    if (class_getSuperclass(objc_getClass("Base")) != Nil) return 6;
    if (!class_isMetaClass(object_getClass((id)objc_getClass("Derived")))) return 7;
    printf("%s %s\n", object_getClassName(d), object_getClassName(base));
    [d destroy];
    [base destroy];
    return [Derived kind] * 10 + 12;                          /* 3 * 10 + 12 = 42 */
}
