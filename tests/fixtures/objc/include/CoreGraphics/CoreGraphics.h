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

/* ---- drawing (CIDER 0.6.1): hand-written declarations of the CoreGraphics subset implemented by cider/src/graphics/cg_api.cpp ---- */
typedef struct CGAffineTransform { CGFloat a, b, c, d, tx, ty; } CGAffineTransform;
extern const CGAffineTransform CGAffineTransformIdentity;
typedef struct CGContext *CGContextRef;
typedef struct CGColor *CGColorRef;
typedef struct CGColorSpace *CGColorSpaceRef;
typedef const struct CGPath *CGPathRef;
typedef struct CGPath *CGMutablePathRef;
typedef struct CGImage *CGImageRef;
typedef enum { kCGPathFill, kCGPathEOFill, kCGPathStroke, kCGPathFillStroke, kCGPathEOFillStroke } CGPathDrawingMode;
typedef enum { kCGLineCapButt, kCGLineCapRound, kCGLineCapSquare } CGLineCap;
typedef enum { kCGLineJoinMiter, kCGLineJoinRound, kCGLineJoinBevel } CGLineJoin;

CGAffineTransform CGAffineTransformMake(CGFloat a, CGFloat b, CGFloat c, CGFloat d, CGFloat tx, CGFloat ty);
CGAffineTransform CGAffineTransformMakeTranslation(CGFloat tx, CGFloat ty);
CGAffineTransform CGAffineTransformMakeScale(CGFloat sx, CGFloat sy);
CGAffineTransform CGAffineTransformMakeRotation(CGFloat angle);
CGAffineTransform CGAffineTransformConcat(CGAffineTransform t1, CGAffineTransform t2);
CGAffineTransform CGAffineTransformTranslate(CGAffineTransform t, CGFloat tx, CGFloat ty);
CGAffineTransform CGAffineTransformScale(CGAffineTransform t, CGFloat sx, CGFloat sy);
CGAffineTransform CGAffineTransformRotate(CGAffineTransform t, CGFloat angle);
CGAffineTransform CGAffineTransformInvert(CGAffineTransform t);
_Bool CGAffineTransformIsIdentity(CGAffineTransform t);
_Bool CGAffineTransformEqualToTransform(CGAffineTransform a, CGAffineTransform b);
CGPoint CGPointApplyAffineTransform(CGPoint point, CGAffineTransform t);
CGSize CGSizeApplyAffineTransform(CGSize size, CGAffineTransform t);
CGRect CGRectApplyAffineTransform(CGRect rect, CGAffineTransform t);

CGColorSpaceRef CGColorSpaceCreateDeviceRGB(void);
CGColorSpaceRef CGColorSpaceRetain(CGColorSpaceRef space);
void CGColorSpaceRelease(CGColorSpaceRef space);
unsigned long CGColorSpaceGetNumberOfComponents(CGColorSpaceRef space);
CGColorRef CGColorCreate(CGColorSpaceRef space, const CGFloat *components);
CGColorRef CGColorCreateGenericRGB(CGFloat red, CGFloat green, CGFloat blue, CGFloat alpha);
CGColorRef CGColorCreateCopy(CGColorRef color);
CGColorRef CGColorRetain(CGColorRef color);
void CGColorRelease(CGColorRef color);
unsigned long CGColorGetNumberOfComponents(CGColorRef color);
const CGFloat *CGColorGetComponents(CGColorRef color);
CGFloat CGColorGetAlpha(CGColorRef color);

CGMutablePathRef CGPathCreateMutable(void);
CGPathRef CGPathCreateCopy(CGPathRef path);
CGMutablePathRef CGPathCreateMutableCopy(CGPathRef path);
CGPathRef CGPathRetain(CGPathRef path);
void CGPathRelease(CGPathRef path);
void CGPathMoveToPoint(CGMutablePathRef path, const CGAffineTransform *m, CGFloat x, CGFloat y);
void CGPathAddLineToPoint(CGMutablePathRef path, const CGAffineTransform *m, CGFloat x, CGFloat y);
void CGPathAddCurveToPoint(CGMutablePathRef path, const CGAffineTransform *m, CGFloat cp1x, CGFloat cp1y, CGFloat cp2x, CGFloat cp2y, CGFloat x, CGFloat y);
void CGPathAddQuadCurveToPoint(CGMutablePathRef path, const CGAffineTransform *m, CGFloat cpx, CGFloat cpy, CGFloat x, CGFloat y);
void CGPathAddRect(CGMutablePathRef path, const CGAffineTransform *m, CGRect rect);
void CGPathAddEllipseInRect(CGMutablePathRef path, const CGAffineTransform *m, CGRect rect);
void CGPathAddArc(CGMutablePathRef path, const CGAffineTransform *m, CGFloat x, CGFloat y, CGFloat radius, CGFloat startAngle, CGFloat endAngle, _Bool clockwise);
void CGPathAddPath(CGMutablePathRef path1, const CGAffineTransform *m, CGPathRef path2);
void CGPathCloseSubpath(CGMutablePathRef path);
_Bool CGPathIsEmpty(CGPathRef path);
CGRect CGPathGetBoundingBox(CGPathRef path);
CGPoint CGPathGetCurrentPoint(CGPathRef path);
_Bool CGPathContainsPoint(CGPathRef path, const CGAffineTransform *m, CGPoint point, _Bool eoFill);
CGPathRef CGPathCreateWithRect(CGRect rect, const CGAffineTransform *transform);
CGPathRef CGPathCreateWithEllipseInRect(CGRect rect, const CGAffineTransform *transform);

unsigned long CGImageGetWidth(CGImageRef image);
unsigned long CGImageGetHeight(CGImageRef image);
CGImageRef CGImageRetain(CGImageRef image);
void CGImageRelease(CGImageRef image);

CGContextRef CGContextRetain(CGContextRef c);
void CGContextRelease(CGContextRef c);
void CGContextSaveGState(CGContextRef c);
void CGContextRestoreGState(CGContextRef c);
void CGContextSetFillColorWithColor(CGContextRef c, CGColorRef color);
void CGContextSetStrokeColorWithColor(CGContextRef c, CGColorRef color);
void CGContextSetRGBFillColor(CGContextRef c, CGFloat red, CGFloat green, CGFloat blue, CGFloat alpha);
void CGContextSetRGBStrokeColor(CGContextRef c, CGFloat red, CGFloat green, CGFloat blue, CGFloat alpha);
void CGContextSetGrayFillColor(CGContextRef c, CGFloat gray, CGFloat alpha);
void CGContextSetGrayStrokeColor(CGContextRef c, CGFloat gray, CGFloat alpha);
void CGContextSetAlpha(CGContextRef c, CGFloat alpha);
void CGContextSetLineWidth(CGContextRef c, CGFloat width);
void CGContextSetLineCap(CGContextRef c, CGLineCap cap);
void CGContextSetLineJoin(CGContextRef c, CGLineJoin join);
void CGContextSetMiterLimit(CGContextRef c, CGFloat limit);
void CGContextSetLineDash(CGContextRef c, CGFloat phase, const CGFloat *lengths, unsigned long count);
void CGContextSetShouldAntialias(CGContextRef c, _Bool shouldAntialias);
void CGContextSetInterpolationQuality(CGContextRef c, int quality);
void CGContextFillRect(CGContextRef c, CGRect rect);
void CGContextStrokeRect(CGContextRef c, CGRect rect);
void CGContextStrokeRectWithWidth(CGContextRef c, CGRect rect, CGFloat width);
void CGContextClearRect(CGContextRef c, CGRect rect);
void CGContextFillEllipseInRect(CGContextRef c, CGRect rect);
void CGContextStrokeEllipseInRect(CGContextRef c, CGRect rect);
void CGContextBeginPath(CGContextRef c);
void CGContextClosePath(CGContextRef c);
void CGContextMoveToPoint(CGContextRef c, CGFloat x, CGFloat y);
void CGContextAddLineToPoint(CGContextRef c, CGFloat x, CGFloat y);
void CGContextAddCurveToPoint(CGContextRef c, CGFloat cp1x, CGFloat cp1y, CGFloat cp2x, CGFloat cp2y, CGFloat x, CGFloat y);
void CGContextAddQuadCurveToPoint(CGContextRef c, CGFloat cpx, CGFloat cpy, CGFloat x, CGFloat y);
void CGContextAddRect(CGContextRef c, CGRect rect);
void CGContextAddRects(CGContextRef c, const CGRect rects[], unsigned long count);
void CGContextAddEllipseInRect(CGContextRef c, CGRect rect);
void CGContextAddArc(CGContextRef c, CGFloat x, CGFloat y, CGFloat radius, CGFloat startAngle, CGFloat endAngle, int clockwise);
void CGContextAddArcToPoint(CGContextRef c, CGFloat x1, CGFloat y1, CGFloat x2, CGFloat y2, CGFloat radius);
void CGContextAddPath(CGContextRef c, CGPathRef path);
void CGContextStrokePath(CGContextRef c);
void CGContextFillPath(CGContextRef c);
void CGContextEOFillPath(CGContextRef c);
void CGContextDrawPath(CGContextRef c, CGPathDrawingMode mode);
void CGContextClip(CGContextRef c);
void CGContextEOClip(CGContextRef c);
void CGContextClipToRect(CGContextRef c, CGRect rect);
void CGContextTranslateCTM(CGContextRef c, CGFloat tx, CGFloat ty);
void CGContextScaleCTM(CGContextRef c, CGFloat sx, CGFloat sy);
void CGContextRotateCTM(CGContextRef c, CGFloat angle);
void CGContextConcatCTM(CGContextRef c, CGAffineTransform transform);
CGAffineTransform CGContextGetCTM(CGContextRef c);
void CGContextDrawImage(CGContextRef c, CGRect rect, CGImageRef image);
void CGContextFlush(CGContextRef c);
void CGContextSynchronize(CGContextRef c);
/* Exported by the real CoreGraphics but NOT implemented by CIDER (a program that imports them is refused at load time; see docs/COREGRAPHICS-RENDERING.md). */
typedef struct CGGradient *CGGradientRef;
void CGContextDrawLinearGradient(CGContextRef c, CGGradientRef gradient, CGPoint startPoint, CGPoint endPoint, unsigned int options);
CGContextRef CGBitmapContextCreate(void *data, unsigned long width, unsigned long height, unsigned long bitsPerComponent, unsigned long bytesPerRow, CGColorSpaceRef space, unsigned int bitmapInfo);
#endif
