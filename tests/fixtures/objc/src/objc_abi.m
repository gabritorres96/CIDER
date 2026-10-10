/* The Objective-C message ABI: every return class travels through objc_msgSend correctly, messages to nil return zeros, variadic methods
   get AL, large structs go through objc_msgSend_stret, many arguments use the stack, and BOOL/char/float/double results are exact. */
#include "objc_min.h"
#include "darwin.h"

struct Pair { long a, b; };
struct Big { long v[5]; };
struct Vec { double x, y; };

__attribute__((objc_root_class))
@interface Kit { Class isa; double scale; }
+ (id)new;
- (double)halve:(double)x;
- (float)third:(float)x;
- (struct Pair)pair:(long)seed;
- (struct Big)big:(long)seed;
- (struct Vec)vec:(double)x;
- (BOOL)isEven:(int)n;
- (char)letter;
- (long)sum8:(long)a :(long)b :(long)c :(long)d :(long)e :(long)f :(long)g :(long)h;
- (double)mix:(int)a d:(double)b i:(int)c d:(double)d;
- (int)sumFirst:(int)count, ...;
- (double)avg:(int)count, ...;
- (void)setScale:(double)s;
- (double)scale;
@end

@implementation Kit
+ (id)new { return class_createInstance(self, 0); }
- (double)halve:(double)x { return x / 2; }
- (float)third:(float)x { return x / 3; }
- (struct Pair)pair:(long)seed { struct Pair p = {seed, seed * 2}; return p; }
- (struct Big)big:(long)seed { struct Big b; for (int i = 0; i < 5; ++i) b.v[i] = seed + i; return b; }
- (struct Vec)vec:(double)x { struct Vec v = {x, x * 2}; return v; }
- (BOOL)isEven:(int)n { return (n % 2) == 0; }
- (char)letter { return 'K'; }
- (long)sum8:(long)a :(long)b :(long)c :(long)d :(long)e :(long)f :(long)g :(long)h { return a + 2 * b + 3 * c + 4 * d + 5 * e + 6 * f + 7 * g + 8 * h; }
- (double)mix:(int)a d:(double)b i:(int)c d:(double)d { return a + b * 2 + c * 3 + d * 4; }
- (int)sumFirst:(int)count, ... {
    va_list ap;
    va_start(ap, count);
    int s = 0;
    for (int i = 0; i < count; ++i) s += va_arg(ap, int);
    va_end(ap);
    return s;
}
- (double)avg:(int)count, ... {
    va_list ap;
    va_start(ap, count);
    double s = 0;
    for (int i = 0; i < count; ++i) s += va_arg(ap, double);
    va_end(ap);
    return s / count;
}
- (void)setScale:(double)s { scale = s; }
- (double)scale { return scale; }
@end

int main(void) {
    Kit *k = [Kit new];
    if ([k halve:9.0] != 4.5) return 1;
    float t = [k third:9.0f];
    if (t < 2.999f || t > 3.001f) return 2;
    struct Pair p = [k pair:21];
    if (p.a != 21 || p.b != 42) return 3;
    struct Big b = [k big:100];                                           /* objc_msgSend_stret */
    if (b.v[0] != 100 || b.v[4] != 104) return 4;
    struct Vec v = [k vec:1.5];                                           /* two SSE eightbytes: xmm0:xmm1 */
    if (v.x != 1.5 || v.y != 3.0) return 5;
    if (![k isEven:10] || [k isEven:7]) return 6;
    if ([k letter] != 'K') return 7;
    if ([k sum8:1 :1 :1 :1 :1 :1 :1 :1] != 36) return 8;                   /* 6th..8th integer arguments on the stack */
    if ([k mix:1 d:2.0 i:3 d:4.0] != 1 + 4 + 9 + 16) return 9;
    if ([k sumFirst:4, 10, 20, 5, 7] != 42) return 10;
    if ([k avg:3, 1.0, 2.0, 6.0] != 3.0) return 11;                       /* AL = 3 vector registers */
    [k setScale:2.5];
    if ([k scale] != 2.5) return 12;

    /* messages to nil: zero in every return register, no crash, arguments evaluated */
    Kit *nothing = (Kit *)0;
    if ([nothing halve:3.0] != 0.0) return 13;
    if ([nothing sum8:1 :2 :3 :4 :5 :6 :7 :8] != 0) return 14;
    struct Pair np = [nothing pair:5];
    if (np.a != 0 || np.b != 0) return 15;
    struct Vec nv = [nothing vec:5.0];
    if (nv.x != 0.0 || nv.y != 0.0) return 16;
    if ([nothing letter] != 0) return 17;
    printf("abi ok\n");
    return 42;
}
