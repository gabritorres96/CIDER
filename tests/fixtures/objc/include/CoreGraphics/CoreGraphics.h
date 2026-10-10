/* Hand-written declarations of the CoreGraphics geometry subset CIDER implements (see docs/COREGRAPHICS.md). Not the Apple header. */
#ifndef CIDER_COREGRAPHICS_H
#define CIDER_COREGRAPHICS_H
typedef double CGFloat;
typedef struct CGPoint { CGFloat x, y; } CGPoint;
typedef struct CGSize { CGFloat width, height; } CGSize;
typedef struct CGRect { CGPoint origin; CGSize size; } CGRect;
typedef enum { CGRectMinXEdge, CGRectMinYEdge, CGRectMaxXEdge, CGRectMaxYEdge } CGRectEdge;
extern const CGPoint CGPointZero;
extern const CGSize CGSizeZero;
extern const CGRect CGRectZero, CGRectNull, CGRectInfinite;
static inline CGPoint CGPointMake(CGFloat x, CGFloat y) { CGPoint p = { x, y }; return p; }
static inline CGSize CGSizeMake(CGFloat w, CGFloat h) { CGSize s = { w, h }; return s; }
static inline CGRect CGRectMake(CGFloat x, CGFloat y, CGFloat w, CGFloat h) { CGRect r = { { x, y }, { w, h } }; return r; }
CGFloat CGRectGetMinX(CGRect r); CGFloat CGRectGetMidX(CGRect r); CGFloat CGRectGetMaxX(CGRect r);
CGFloat CGRectGetMinY(CGRect r); CGFloat CGRectGetMidY(CGRect r); CGFloat CGRectGetMaxY(CGRect r);
CGFloat CGRectGetWidth(CGRect r); CGFloat CGRectGetHeight(CGRect r);
_Bool CGPointEqualToPoint(CGPoint a, CGPoint b); _Bool CGSizeEqualToSize(CGSize a, CGSize b); _Bool CGRectEqualToRect(CGRect a, CGRect b);
_Bool CGRectIsEmpty(CGRect r); _Bool CGRectIsNull(CGRect r); _Bool CGRectIsInfinite(CGRect r);
_Bool CGRectContainsPoint(CGRect r, CGPoint p); _Bool CGRectContainsRect(CGRect a, CGRect b); _Bool CGRectIntersectsRect(CGRect a, CGRect b);
CGRect CGRectStandardize(CGRect r); CGRect CGRectIntegral(CGRect r);
CGRect CGRectInset(CGRect r, CGFloat dx, CGFloat dy); CGRect CGRectOffset(CGRect r, CGFloat dx, CGFloat dy);
CGRect CGRectUnion(CGRect a, CGRect b); CGRect CGRectIntersection(CGRect a, CGRect b);
void CGRectDivide(CGRect r, CGRect *slice, CGRect *remainder, CGFloat amount, CGRectEdge edge);
#endif
