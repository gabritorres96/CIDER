/* CIDER 0.6 CoreGraphics ABI test: a macOS x86_64 program calling the CoreGraphics functions that pass or return structures, so the arguments are laid out by
   clang for the System V ABI, not by the host:
     CGPoint / CGSize        two SSE eightbytes (xmm0:xmm1, xmm2:xmm3 ...), returned in xmm0:xmm1
     CGRect (32 bytes)       class MEMORY: copied on the stack; returned through a hidden pointer in rdi
     CGAffineTransform (48)  class MEMORY: copied on the stack; returned through a hidden pointer in rdi
   It also checks that callee-saved registers survive the calls (values kept live across thousands of calls), that invalid handles are refused without a crash
   (the host counts them: exactly 11 here), and that the CF retain/release functions work on CoreGraphics objects. Exit code = number of failures. */
#include "AppKit/AppKit.h"

#define PI 3.14159265358979323846

static int failures;
#define CHECK(...) do { if (!(__VA_ARGS__)) { failures++; NSLog(@"FAIL line %d: %s", __LINE__, #__VA_ARGS__); } } while (0)

static int feq(double a, double b) { double d = a - b; return d < 1e-9 && d > -1e-9; }
static int teq(CGAffineTransform t, double a, double b, double c, double d, double tx, double ty) {
    return feq(t.a, a) && feq(t.b, b) && feq(t.c, c) && feq(t.d, d) && feq(t.tx, tx) && feq(t.ty, ty);
}

extern void CFRelease(void *);
extern void *CFRetain(void *);
extern long CFGetRetainCount(void *);

int main(int argc, const char *argv[]) {
    @autoreleasepool {
        /* --- CGAffineTransform: 48 bytes by value, in and out --- */
        CGAffineTransform id = CGAffineTransformIdentity;
        CHECK(teq(id, 1, 0, 0, 1, 0, 0) && CGAffineTransformIsIdentity(id));
        CGAffineTransform m = CGAffineTransformMake(2, 3, 5, 7, 11, 13);
        CHECK(teq(m, 2, 3, 5, 7, 11, 13) && !CGAffineTransformIsIdentity(m));
        CGAffineTransform t = CGAffineTransformMakeTranslation(10, 20);
        CGAffineTransform s = CGAffineTransformMakeScale(2, 3);
        CGAffineTransform cat = CGAffineTransformConcat(t, s);         /* t first, then s: (x + 10) * 2, (y + 20) * 3 */
        CHECK(teq(cat, 2, 0, 0, 3, 20, 60));
        CGPoint p = CGPointApplyAffineTransform(CGPointMake(1, 1), cat);
        CHECK(feq(p.x, 22) && feq(p.y, 63));
        CGAffineTransform tr = CGAffineTransformTranslate(s, 10, 20);    /* translate first, then s */
        CHECK(teq(tr, 2, 0, 0, 3, 20, 60));
        CGAffineTransform sc = CGAffineTransformScale(t, 2, 3);          /* scale first, then t */
        CHECK(teq(sc, 2, 0, 0, 3, 10, 20));
        CGAffineTransform rot = CGAffineTransformRotate(CGAffineTransformIdentity, PI / 2);
        CGPoint rp = CGPointApplyAffineTransform(CGPointMake(1, 0), rot);
        CHECK(feq(rp.x, 0) && feq(rp.y, 1));
        CGAffineTransform mr = CGAffineTransformMakeRotation(PI);
        CHECK(feq(mr.a, -1) && feq(mr.d, -1) && feq(mr.b, 0) && feq(mr.c, 0));
        CGAffineTransform inv = CGAffineTransformInvert(cat);
        CGPoint back = CGPointApplyAffineTransform(p, inv);
        CHECK(feq(back.x, 1) && feq(back.y, 1));
        CGAffineTransform singular = CGAffineTransformMake(0, 0, 0, 0, 5, 5);
        CHECK(CGAffineTransformEqualToTransform(CGAffineTransformInvert(singular), singular));   /* a singular matrix comes back unchanged */
        CHECK(CGAffineTransformEqualToTransform(m, m) && !CGAffineTransformEqualToTransform(m, t));
        CGSize sz = CGSizeApplyAffineTransform(CGSizeMake(4, 5), s);
        CHECK(feq(sz.width, 8) && feq(sz.height, 15));
        CGRect rr = CGRectApplyAffineTransform(CGRectMake(1, 2, 3, 4), cat);       /* CGRect in, CGAffineTransform in, CGRect out */
        CHECK(feq(rr.origin.x, 22) && feq(rr.origin.y, 66) && feq(rr.size.width, 6) && feq(rr.size.height, 12));
        CGRect rot_bounds = CGRectApplyAffineTransform(CGRectMake(0, 0, 10, 0), CGAffineTransformMakeRotation(PI / 2));
        CHECK(feq(rot_bounds.size.width, 0) && feq(rot_bounds.size.height, 10));

        /* --- CGPath: pointers, struct pointers, rects and points --- */
        CGMutablePathRef path = CGPathCreateMutable();
        CHECK(path != NULL && CGPathIsEmpty(path));
        CGAffineTransform shift = CGAffineTransformMakeTranslation(100, 200);
        CGPathAddRect(path, &shift, CGRectMake(1, 2, 30, 40));          /* ptr, ptr, CGRect on the stack */
        CHECK(!CGPathIsEmpty(path));
        CGRect box = CGPathGetBoundingBox(path);                          /* hidden result pointer + path */
        CHECK(feq(box.origin.x, 101) && feq(box.origin.y, 202) && feq(box.size.width, 30) && feq(box.size.height, 40));
        CGPoint cur = CGPathGetCurrentPoint(path);                        /* CGPoint result in xmm0:xmm1 */
        CHECK(feq(cur.x, 101) && feq(cur.y, 202));
        CHECK(CGPathContainsPoint(path, NULL, CGPointMake(110, 210), 0));        /* ptr, ptr, CGPoint (SSE), bool (int register) */
        CHECK(!CGPathContainsPoint(path, NULL, CGPointMake(10, 10), 0));
        CHECK(CGPathContainsPoint(path, &shift, CGPointMake(210, 410), 0) && !CGPathContainsPoint(path, &shift, CGPointMake(10, 10), 0));   /* the path is transformed by m again before the test */
        CGPathMoveToPoint(path, NULL, 0, 0);
        CGPathAddArc(path, NULL, 0, 0, 50, 0, PI, 0);                /* 6 doubles + 2 ints: no stack arguments */
        CGPathAddCurveToPoint(path, NULL, 1, 2, 3, 4, 5, 6);              /* 6 doubles: xmm0..xmm5 */
        CGRect box2 = CGPathGetBoundingBox(path);
        CHECK(box2.size.width >= 150 && box2.origin.x <= 0);
        CGPathRef rectPath = CGPathCreateWithRect(CGRectMake(5, 6, 7, 8), NULL);     /* CGRect on the stack, pointer in rdi */
        CGRect rb = CGPathGetBoundingBox(rectPath);
        CHECK(feq(rb.origin.x, 5) && feq(rb.size.height, 8));
        CGPathRef ellipse = CGPathCreateWithEllipseInRect(CGRectMake(0, 0, 100, 50), NULL);
        CHECK(CGPathContainsPoint(ellipse, NULL, CGPointMake(50, 25), 0) && !CGPathContainsPoint(ellipse, NULL, CGPointMake(2, 2), 0));
        CGMutablePathRef copy = CGPathCreateMutableCopy(path);
        CGPathRef frozen = CGPathCreateCopy(path);
        CHECK(copy != NULL && frozen != NULL && copy != path);
        CGPathAddPath(copy, NULL, rectPath);
        CGPathRelease(frozen);
        CGPathRelease(copy);
        CGPathRelease(ellipse);
        CGPathRelease(rectPath);
        CGPathRelease(path);

        /* --- callee-saved registers across thousands of calls --- */
        long a1 = 1, a2 = 2, a3 = 3, a4 = 4, a5 = 5, a6 = 6;
        double d1 = 1.5, d2 = 2.5;
        for (int i = 0; i < 5000; i++) {
            CGAffineTransform x = CGAffineTransformMakeRotation((double)i);
            CGPoint q = CGPointApplyAffineTransform(CGPointMake(1, 0), x);
            a1 += (long)(q.x > 2); a2 += 1; a3 ^= i; a4 += a3 & 1; a5 += a1; a6 += a2;
            d1 += q.x; d2 += q.y;
        }
        long s1 = 1, s2 = 2, s3 = 3, s4 = 4, s5 = 5, s6 = 6;                /* the same arithmetic without any call in between */
        for (int i = 0; i < 5000; i++) { s2 += 1; s3 ^= i; s4 += s3 & 1; s5 += s1; s6 += s2; }
        CHECK(a1 == s1 && a2 == s2 && a3 == s3 && a4 == s4 && a5 == s5 && a6 == s6);
        CHECK(d1 > -100 && d1 < 100 && d2 > -100 && d2 < 100);

        /* --- retain counts through the CF functions and the CG functions --- */
        CGColorSpaceRef rgb = CGColorSpaceCreateDeviceRGB();
        CGFloat comps[4] = { 0.25, 0.5, 0.75, 1.0 };
        CGColorRef c = CGColorCreate(rgb, comps);
        CHECK(CFGetRetainCount(c) == 1);
        CFRetain(c);
        CHECK(CFGetRetainCount(c) == 2);
        CGColorRetain(c);
        CHECK(CFGetRetainCount(c) == 3);
        CGColorRelease(c);
        CFRelease(c);
        CHECK(CFGetRetainCount(c) == 1);
        CGColorRef cc = CGColorCreateCopy(c);
        CHECK(cc != c && CGColorGetNumberOfComponents(cc) == 4 && CGColorGetComponents(cc)[1] == 0.5 && CGColorGetAlpha(cc) == 1.0);
        CGColorRelease(cc);
        CGColorRelease(c);
        CGColorSpaceRelease(rgb);

        /* --- invalid handles: refused and counted by the host, never a crash --- */
        NSString *notACGObject = @"not a CoreGraphics object";
        CGContextSaveGState(NULL);                                         /* nil context */
        CGContextFillRect((CGContextRef)(__bridge void *)notACGObject, CGRectMake(0, 0, 1, 1));     /* a live object of the wrong class */
        CGContextSetLineWidth((CGContextRef)(void *)0x1234, 2);            /* garbage pointer */
        CGColorRelease((CGColorRef)(__bridge void *)notACGObject);
        CGPathAddRect((CGMutablePathRef)(__bridge void *)notACGObject, NULL, CGRectMake(0, 0, 1, 1));
        CGPathRef immutable = CGPathCreateWithRect(CGRectMake(0, 0, 1, 1), NULL);
        CGPathAddLineToPoint((CGMutablePathRef)immutable, NULL, 5, 5);     /* an immutable path is not a CGMutablePathRef */
        CHECK(CGPathGetBoundingBox(immutable).size.width == 1);
        CGPathRelease(immutable);
        CHECK(CGColorGetNumberOfComponents(NULL) == 0);
        CHECK(CGImageGetWidth(NULL) == 0);
        CGColorSpaceRelease(NULL);                                         /* NULL release is a legal no-op, not an error */
        CGColorRelease(NULL);

        /* dead handles: a second release and a use after release are refused (no crash, no double free) */
        CGColorSpaceRef sp2 = CGColorSpaceCreateDeviceRGB();
        const CGFloat c2[4] = { 1, 1, 1, 1 };
        CGColorRef dead = CGColorCreate(sp2, c2);
        CGColorRelease(dead);
        CGColorRelease(dead);                                              /* double release */
        CGMutablePathRef deadPath = CGPathCreateMutable();
        CGPathRelease(deadPath);
        CHECK(CGPathIsEmpty(deadPath));                                    /* use after release: refused, reports empty */
        CGPathRelease(deadPath);
        CGColorSpaceRelease(sp2);
    }
    NSLog(@"cg_abi_test done: failures=%d", failures);
    return failures;
}
