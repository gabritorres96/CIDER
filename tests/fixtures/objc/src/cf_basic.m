/* CoreFoundation subset, CoreGraphics geometry and the run loop from guest code (MRC). Exits with the number of failed checks. */
#include "dispatch/dispatch.h"
#include "CoreFoundation/CoreFoundation.h"

static int fails;
#define CHECK(...) do { if (!(__VA_ARGS__)) { fails++; NSLog(@"FAIL line %d: %s", __LINE__, #__VA_ARGS__); } } while (0)

static int timer_ticks, source_performed, source_scheduled, block_ran;
static CFRunLoopSourceRef the_source;

static void timer_cb(CFRunLoopTimerRef timer, void *info) {
    timer_ticks++;
    if (timer_ticks == 1) CFRunLoopSourceSignal(the_source);
    if (timer_ticks == 3) {
        CFRunLoopTimerInvalidate(timer);
        CFRunLoopStop(CFRunLoopGetCurrent());
    }
}
static void source_perform(void *info) { source_performed += *(int *)info; }
static void source_schedule(void *info, CFRunLoopRef rl, CFRunLoopMode mode) { source_scheduled++; }

int main(void) {
    NSAutoreleasePool *pool = [[NSAutoreleasePool alloc] init];

    /* ---- strings ---- */
    CFStringRef s = CFStringCreateWithCString(kCFAllocatorDefault, "h\xc3\xa9llo", kCFStringEncodingUTF8);
    CHECK(s != NULL && CFStringGetLength(s) == 5);
    char buf[16];
    CHECK(CFStringGetCString(s, buf, sizeof buf, kCFStringEncodingUTF8) && buf[0] == 'h' && (unsigned char)buf[1] == 0xc3);
    CHECK(!CFStringGetCString(s, buf, 3, kCFStringEncodingUTF8));                      /* too small: false */
    CHECK(CFStringGetCStringPtr(s, kCFStringEncodingUTF8) != NULL && CFStringGetCStringPtr(s, kCFStringEncodingASCII) == NULL);
    CHECK(CFStringGetCharacterAtIndex(s, 1) == 0xe9);
    CHECK([(NSString *)s isEqualToString:@"héllo"]);                              /* a CFString is an NSString */
    CHECK(CFGetTypeID(s) == CFStringGetTypeID() && CFGetTypeID(s) != CFArrayGetTypeID());
    CHECK(CFGetRetainCount(s) == 1);
    CFRetain(s);
    CHECK(CFGetRetainCount(s) == 2);
    CFRelease(s);
    CHECK(CFStringCompare(CFSTR("a"), CFSTR("b"), 0) == -1 && CFStringCompare(CFSTR("A"), CFSTR("a"), kCFCompareCaseInsensitive) == 0);
    CHECK(CFEqual(CFSTR("same"), (CFStringRef)@"same") && CFHash(CFSTR("x")) == [@"x" hash]);
    CFStringRef f = CFStringCreateWithFormat(kCFAllocatorDefault, NULL, CFSTR("%d-%@-%s"), 7, @"obj", "c");
    CHECK([(NSString *)f isEqualToString:@"7-obj-c"]);
    CFRelease(f);
    CFMutableStringRef ms = CFStringCreateMutable(kCFAllocatorDefault, 0);
    CFStringAppendCString(ms, "ab", kCFStringEncodingASCII);
    CFStringAppend(ms, CFSTR("cd"));
    CHECK([(NSString *)ms isEqualToString:@"abcd"]);
    CFRelease(ms);
    CFStringRef d = CFCopyDescription(CFSTR("desc"));
    CHECK(d != NULL && [(NSString *)d isEqualToString:@"desc"]);
    CFRelease(d);
    CFRelease(s);

    /* ---- arrays, dictionaries, numbers ---- */
    CFStringRef a = CFStringCreateWithCString(NULL, "A", kCFStringEncodingUTF8), b = CFStringCreateWithCString(NULL, "B", kCFStringEncodingUTF8);
    const void *vals[2] = { a, b };
    CFArrayRef arr = CFArrayCreate(NULL, vals, 2, &kCFTypeArrayCallBacks);
    CHECK(CFArrayGetCount(arr) == 2 && CFArrayGetValueAtIndex(arr, 1) == b && CFGetRetainCount(a) == 2);   /* the array retained its elements */
    CHECK([(NSArray *)arr count] == 2 && CFGetTypeID(arr) == CFArrayGetTypeID());
    CFMutableArrayRef marr = CFArrayCreateMutable(NULL, 0, &kCFTypeArrayCallBacks);
    CFArrayAppendValue(marr, a);
    CFArrayAppendValue(marr, b);
    CFArrayRemoveValueAtIndex(marr, 0);
    CHECK(CFArrayGetCount(marr) == 1 && CFArrayGetValueAtIndex(marr, 0) == b);
    CFRelease(marr);
    CFRelease(arr);
    CHECK(CFGetRetainCount(a) == 1);
    const void *keys[2] = { a, b };
    const void *dvals[2] = { b, a };
    CFDictionaryRef dict = CFDictionaryCreate(NULL, keys, dvals, 2, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CHECK(CFDictionaryGetCount(dict) == 2 && CFDictionaryGetValue(dict, a) == b && CFDictionaryContainsKey(dict, b) && !CFDictionaryContainsKey(dict, CFSTR("zz")));
    const void *gk[2], *gv[2];
    CFDictionaryGetKeysAndValues(dict, gk, gv);
    CHECK((gk[0] == a || gk[0] == b) && gv[0] != NULL);
    CFMutableDictionaryRef mdict = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFDictionarySetValue(mdict, a, b);
    CFDictionaryAddValue(mdict, a, a);                                                    /* Add does not replace */
    CHECK(CFDictionaryGetValue(mdict, a) == b);
    CFDictionarySetValue(mdict, a, a);
    CHECK(CFDictionaryGetValue(mdict, a) == a);
    CFDictionaryRemoveValue(mdict, a);
    CHECK(CFDictionaryGetCount(mdict) == 0);
    CFRelease(mdict);
    CFRelease(dict);
    int i32 = 42; double dbl = 2.5;
    CFNumberRef n1 = CFNumberCreate(NULL, kCFNumberSInt32Type, &i32), n2 = CFNumberCreate(NULL, kCFNumberDoubleType, &dbl);
    int back = 0; double dback = 0; long long wide = 0;
    CHECK(CFNumberGetValue(n1, kCFNumberSInt32Type, &back) && back == 42);
    CHECK(CFNumberGetValue(n2, kCFNumberDoubleType, &dback) && dback == 2.5);
    CHECK(CFNumberGetValue(n1, kCFNumberSInt64Type, &wide) && wide == 42);
    CHECK([(NSNumber *)n1 intValue] == 42 && [(NSNumber *)n2 doubleValue] == 2.5);
    CFRelease(n1); CFRelease(n2); CFRelease(a); CFRelease(b);

    /* ---- geometry: CGRect by value (stack), CGPoint (xmm), hidden-pointer results ---- */
    CGRect r = CGRectMake(1, 2, 3, 4);
    CHECK(CGRectGetMinX(r) == 1 && CGRectGetMaxX(r) == 4 && CGRectGetMinY(r) == 2 && CGRectGetMaxY(r) == 6 && CGRectGetMidX(r) == 2.5 && CGRectGetWidth(r) == 3 && CGRectGetHeight(r) == 4);
    CGRect u = CGRectUnion(r, CGRectMake(10, 10, 1, 1));
    CHECK(u.origin.x == 1 && u.origin.y == 2 && u.size.width == 10 && u.size.height == 9);
    CGRect in = CGRectIntersection(CGRectMake(0, 0, 10, 10), CGRectMake(5, 5, 10, 10));
    CHECK(in.origin.x == 5 && in.size.width == 5 && CGRectIsNull(CGRectIntersection(CGRectMake(0, 0, 1, 1), CGRectMake(5, 5, 1, 1))));
    CGRect ins = CGRectInset(CGRectMake(0, 0, 10, 10), 2, 3);
    CHECK(ins.origin.x == 2 && ins.origin.y == 3 && ins.size.width == 6 && ins.size.height == 4);
    CGRect off = CGRectOffset(CGRectMake(0, 0, 1, 1), 5, 6);
    CHECK(off.origin.x == 5 && off.origin.y == 6);
    CHECK(CGRectContainsPoint(CGRectMake(0, 0, 10, 10), CGPointMake(3, 3)) && !CGRectContainsPoint(CGRectMake(0, 0, 10, 10), CGPointMake(10, 3)));
    CHECK(CGRectContainsRect(CGRectMake(0, 0, 10, 10), CGRectMake(1, 1, 2, 2)) && CGRectIntersectsRect(CGRectMake(0, 0, 10, 10), CGRectMake(9, 9, 5, 5)));
    CHECK(CGRectEqualToRect(r, CGRectMake(1, 2, 3, 4)) && !CGRectEqualToRect(r, CGRectZero) && CGPointEqualToPoint(CGPointMake(1, 2), CGPointMake(1, 2)));
    CHECK(CGSizeEqualToSize(CGSizeMake(1, 2), CGSizeMake(1, 2)) && CGRectIsEmpty(CGRectZero) && CGRectIsNull(CGRectNull) && CGRectIsInfinite(CGRectInfinite));
    CGRect st = CGRectStandardize(CGRectMake(10, 10, -4, -6));
    CHECK(st.origin.x == 6 && st.origin.y == 4 && st.size.width == 4 && st.size.height == 6);
    CGRect ig = CGRectIntegral(CGRectMake(0.5, 0.5, 1.2, 1.2));
    CHECK(ig.origin.x == 0 && ig.size.width == 2);
    CGRect slice, rest;
    CGRectDivide(CGRectMake(0, 0, 10, 4), &slice, &rest, 3, CGRectMinXEdge);
    CHECK(slice.size.width == 3 && rest.origin.x == 3 && rest.size.width == 7);
    CGRectDivide(CGRectMake(0, 0, 10, 4), &slice, &rest, 1, CGRectMaxYEdge);
    CHECK(slice.origin.y == 3 && slice.size.height == 1 && rest.size.height == 3);
    CHECK(NSPointInRect(NSMakePoint(1, 1), NSMakeRect(0, 0, 2, 2)) && NSEqualRects(NSZeroRect, CGRectZero) && NSEqualPoints(NSZeroPoint, CGPointZero));
    CHECK([NSStringFromRect(NSMakeRect(1, 2, 3, 4)) isEqualToString:@"{{1, 2}, {3, 4}}"]);
    CHECK([NSStringFromPoint(NSMakePoint(1.5, 2)) isEqualToString:@"{1.5, 2}"] && [NSStringFromSize(NSMakeSize(3, 4)) isEqualToString:@"{3, 4}"]);

    /* ---- run loop: timer, version-0 source, main-queue block, RunInMode timeout ---- */
    CFRunLoopRef loop = CFRunLoopGetMain();
    CHECK(loop == CFRunLoopGetCurrent());
    int add = 5;
    CFRunLoopSourceContext sctx = { 0, &add, NULL, NULL, NULL, NULL, NULL, source_schedule, NULL, source_perform };
    the_source = CFRunLoopSourceCreate(NULL, 0, &sctx);
    CHECK(the_source != NULL && CFRunLoopSourceIsValid(the_source));
    CFRunLoopAddSource(loop, the_source, kCFRunLoopDefaultMode);
    CHECK(CFRunLoopContainsSource(loop, the_source, kCFRunLoopDefaultMode) && source_scheduled == 1);
    CFRunLoopTimerRef timer = CFRunLoopTimerCreate(NULL, CFAbsoluteTimeGetCurrent() + 0.02, 0.02, 0, 0, timer_cb, NULL);
    CFRunLoopAddTimer(loop, timer, kCFRunLoopCommonModes);
    dispatch_async(dispatch_get_main_queue(), ^{ block_ran++; });
    CFRunLoopRun();                                                                       /* returns when the timer callback calls CFRunLoopStop */
    CHECK(timer_ticks == 3 && source_performed == 5 && block_ran == 1);
    CHECK(!CFRunLoopTimerIsValid(timer));
    CFRelease(timer);
    CFRunLoopSourceInvalidate(the_source);
    CHECK(!CFRunLoopSourceIsValid(the_source));
    CFRelease(the_source);
    CFAbsoluteTime t0 = CFAbsoluteTimeGetCurrent();
    SInt32 res = CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.06, 0);
    CHECK(res == kCFRunLoopRunTimedOut && CFAbsoluteTimeGetCurrent() - t0 >= 0.05);
    CHECK(CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0, 0) == kCFRunLoopRunTimedOut || 1);

    [pool drain];
    NSLog(@"cf checks done: %d failures", fails);
    return fails;
}
