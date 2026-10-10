/* Message forwarding (MRC): dynamic method resolution, forwarding targets and invocations. Exits with the number of failed checks.
   argv[1] (optional): s = forward a selector whose signature has a struct argument (must end the guest), u = send a selector nobody handles. */
#include "Foundation/Foundation.h"

static int fails;
#define CHECK(...) do { if (!(__VA_ARGS__)) { fails++; NSLog(@"FAIL line %d: %s", __LINE__, #__VA_ARGS__); } } while (0)

static int dynamic_instance_imp(id self, SEL _cmd) { return 7; }
static id dynamic_class_imp(id self, SEL _cmd) { return @"class-resolved"; }

@interface Worker : NSObject
- (int)doubleIt:(int)x;
- (int)add:(int)a to:(int)b;
- (double)scale:(double)v by:(double)f;
- (NSString *)label:(NSString *)prefix with:(long)n;
- (float)half:(float)v;
@end
@implementation Worker
- (int)doubleIt:(int)x { return x * 2; }
- (int)add:(int)a to:(int)b { return a + b; }
- (double)scale:(double)v by:(double)f { return v * f; }
- (NSString *)label:(NSString *)prefix with:(long)n { return [NSString stringWithFormat:@"%@-%ld", prefix, n]; }
- (float)half:(float)v { return v / 2; }
@end

@interface Resolver : NSObject
- (int)dynamicMethod;
+ (id)dynamicClassMethod;
@end
@implementation Resolver
+ (BOOL)resolveInstanceMethod:(SEL)sel {
    if (sel == @selector(dynamicMethod)) { class_addMethod([self class], sel, (IMP)dynamic_instance_imp, "i16@0:8"); return YES; }
    return [super resolveInstanceMethod:sel];
}
+ (BOOL)resolveClassMethod:(SEL)sel {
    if (sel == @selector(dynamicClassMethod)) { class_addMethod(object_getClass((id)self), sel, (IMP)dynamic_class_imp, "@16@0:8"); return YES; }
    return [super resolveClassMethod:sel];
}
@end

@interface TargetProxy : NSObject {
    Worker *worker;
}
- (id)initWith:(Worker *)w;
@end
@implementation TargetProxy
- (id)initWith:(Worker *)w { self = [super init]; worker = w; return self; }
- (id)forwardingTargetForSelector:(SEL)sel { return [worker respondsToSelector:sel] ? worker : nil; }
@end

static int invocations;
@interface InvocationProxy : NSObject {
    Worker *worker;
}
- (id)initWith:(Worker *)w;
@end
@implementation InvocationProxy
- (id)initWith:(Worker *)w { self = [super init]; worker = w; return self; }
- (NSMethodSignature *)methodSignatureForSelector:(SEL)sel {
    NSMethodSignature *s = [worker methodSignatureForSelector:sel];
    return s ? s : [super methodSignatureForSelector:sel];
}
- (void)forwardInvocation:(NSInvocation *)inv {
    invocations++;
    SEL sel = [inv selector];
    if (sel == @selector(doubleIt:)) {                /* edit an argument, call the real method, edit the result */
        int x; [inv getArgument:&x atIndex:2];
        x += 1; [inv setArgument:&x atIndex:2];
        [inv invokeWithTarget:worker];
        int r; [inv getReturnValue:&r];
        r += 100; [inv setReturnValue:&r];
    } else if ([worker respondsToSelector:sel]) {
        [inv invokeWithTarget:worker];
    } else {
        [super forwardInvocation:inv];
    }
}
@end

@interface StructTaker : NSObject
- (void)takeRect:(NSRect)r;
@end
@implementation StructTaker
- (void)takeRect:(NSRect)r {}
@end
@interface StructProxy : NSObject {
    StructTaker *t;
}
- (id)initWith:(StructTaker *)x;
@end
@implementation StructProxy
- (id)initWith:(StructTaker *)x { self = [super init]; t = x; return self; }
- (NSMethodSignature *)methodSignatureForSelector:(SEL)sel { return [t methodSignatureForSelector:sel]; }
- (void)forwardInvocation:(NSInvocation *)inv { [inv invokeWithTarget:t]; }
@end

int main(int argc, char **argv) {
    char mode = argc > 1 ? argv[1][0] : 0;
    NSAutoreleasePool *pool = [[NSAutoreleasePool alloc] init];
    Worker *worker = [[Worker alloc] init];

    /* +resolveInstanceMethod: / +resolveClassMethod: */
    Resolver *res = [[Resolver alloc] init];
    CHECK(![res respondsToSelector:@selector(dynamicMethod)]);
    CHECK([res dynamicMethod] == 7);
    CHECK([res respondsToSelector:@selector(dynamicMethod)]);
    CHECK([[Resolver dynamicClassMethod] isEqualToString:@"class-resolved"]);

    /* -forwardingTargetForSelector: replays the call on another receiver (integer, double, float, object and long arguments) */
    id proxy = [[TargetProxy alloc] initWith:worker];
    CHECK([proxy doubleIt:21] == 42);
    CHECK([proxy add:40 to:2] == 42);
    CHECK([proxy scale:1.5 by:4.0] == 6.0);
    CHECK([proxy half:5.0f] == 2.5f);
    CHECK([[proxy label:@"n" with:-9L] isEqualToString:@"n--9"]);

    /* -forwardInvocation: */
    id ip = [[InvocationProxy alloc] initWith:worker];
    CHECK([ip doubleIt:10] == 122);                     /* (10 + 1) * 2 + 100 */
    CHECK([ip add:1 to:2] == 3);
    CHECK([ip scale:2.0 by:2.5] == 5.0);
    CHECK([[ip label:@"x" with:3L] isEqualToString:@"x-3"]);
    CHECK(invocations == 4);

    /* NSMethodSignature / NSInvocation used directly */
    NSMethodSignature *sig = [worker methodSignatureForSelector:@selector(add:to:)];
    CHECK(sig != nil && [sig numberOfArguments] == 4 && [sig methodReturnType][0] == 'i' && [sig methodReturnLength] == 4);
    CHECK([sig getArgumentTypeAtIndex:0][0] == '@' && [sig getArgumentTypeAtIndex:1][0] == ':' && [sig getArgumentTypeAtIndex:2][0] == 'i');
    CHECK([worker methodSignatureForSelector:@selector(noSuchSelector)] == nil);
    NSInvocation *inv = [NSInvocation invocationWithMethodSignature:sig];
    int a = 30, b = 12;
    [inv setTarget:worker]; [inv setSelector:@selector(add:to:)];
    [inv setArgument:&a atIndex:2]; [inv setArgument:&b atIndex:3];
    [inv invoke];
    int sum = 0; [inv getReturnValue:&sum];
    CHECK(sum == 42 && [inv target] == worker && [inv selector] == @selector(add:to:));
    NSMethodSignature *dsig = [NSMethodSignature signatureWithObjCTypes:"d32@0:8d16d24"];
    CHECK([dsig numberOfArguments] == 4 && [dsig methodReturnType][0] == 'd');

    if (mode == 's') {
        StructProxy *sp = [[StructProxy alloc] initWith:[[StructTaker alloc] init]];
        [(id)sp takeRect:NSMakeRect(1, 2, 3, 4)];
        NSLog(@"not reached");
    }
    if (mode == 'u') [(id)worker performSelector:@selector(nobodyHandlesThis)];

    [pool drain];
    NSLog(@"forward checks done: %d failures", fails);
    return fails;
}
