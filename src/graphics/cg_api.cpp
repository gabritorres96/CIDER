// Guest-visible CoreGraphics (CIDER 0.6.1): CGContext, CGColor, CGColorSpace, CGPath, CGImage and CGAffineTransform functions, implemented over
// the engine in cider/cg.hpp. The signatures follow Apple's public CoreGraphics API (names, argument order, struct passing); nothing of Apple's code is used.
//
// System V passing as clang emits it for macOS x86_64:
//   * CGFloat is double; CGPoint / CGSize are two SSE eightbytes (two xmm registers); CGRect (32 bytes) and CGAffineTransform (48 bytes) are class MEMORY:
//     passed by value on the stack in argument order, returned through a hidden pointer in rdi (so the real arguments start at rsi).
//   * `const CGAffineTransform *` arguments are ordinary pointers.
// Every function is a raw handler that walks the registers and the stack with sysv::Args.
//
// Not implemented (absent from the exported symbol table, so a program that needs one is refused at link time, not given a fake): CGBitmapContext*,
// CGPDFContext*, gradients, shadings, patterns, layers, CGContextSetBlendMode, CGContextDrawLinearGradient, text drawing through CGContext, color
// spaces other than DeviceRGB, CGColorConvert, CGPathApply, CGPathCreateCopyByStrokingPath. See docs/COREGRAPHICS-RENDERING.md.
#include <atomic>
#include <cstring>
#include <format>

#include "cider/abi.hpp"
#include "cider/cg.hpp"
#include "cider/libsystem.hpp"
#include "cider/log.hpp"
#include "cider/objc.hpp"
#include "../foundation/foundation_internal.hpp"
#include "../libsystem/internal.hpp"
#include "cg_api.hpp"
#include "sysv_args.hpp"

namespace cider::graphics {
namespace {

namespace fnd = cider::foundation;
using abi::GuestRegs;
using cg::Affine;
using geometry::Point;
using geometry::Rect;
using objc::Addr;
using objc::Class;
using sysv::Args;

constexpr double kPi = 3.14159265358979323846;

std::atomic<uint64_t> g_invalid{0};
std::atomic<int64_t> g_live{0};

struct CgPayload : fnd::Payload {
    CgPayload() { ++g_live; }
    ~CgPayload() override { --g_live; }
};
struct SpacePayload : CgPayload {};
struct ColorPayload : CgPayload {
    double comps[4] = {0, 0, 0, 1};                     // r, g, b, a (DeviceRGB)
};
struct PathPayload : CgPayload {
    cg::Path path;
    bool mutable_path = false;
};
struct ImagePayload : CgPayload { cg::Image image; };
struct ContextPayload : CgPayload {
    cg::Device* device = nullptr;
    std::unique_ptr<cg::Context> engine;                // null once the repaint that owns the device ended
};

Class *g_space = nullptr, *g_color = nullptr, *g_path = nullptr, *g_image = nullptr, *g_context = nullptr;

Addr g_identity = 0;                                    // storage of CGAffineTransformIdentity

fnd::Runtime& rt() { return fnd::rt(); }

void refuse(const char* fn, const char* why, Addr handle) {
    ++g_invalid;
    log::warn(log::Category::Objc, "{}: {} ({:#x}); the call is ignored", fn, why, handle);
}

template <class T>
T* typed(Addr obj, Class* cls) {
    if (!obj || !cls || !fnd::is_kind(obj, cls)) return nullptr;
    return dynamic_cast<T*>(fnd::payload_of(obj));
}

Addr make(Class* cls, std::unique_ptr<fnd::Payload> p) { return fnd::make_object(cls, std::move(p)); }

// ---- argument checks -------------------------------------------------------------------------------------------------------------------------------
cg::Context* use_ctx(Addr h, const char* fn) {
    ContextPayload* c = typed<ContextPayload>(h, g_context);
    if (!c) { refuse(fn, "invalid context", h); return nullptr; }
    if (!c->engine) { refuse(fn, "the context is no longer valid (its drawing pass ended)", h); return nullptr; }
    return c->engine.get();
}
PathPayload* use_path(Addr h, const char* fn, bool need_mutable) {
    PathPayload* p = typed<PathPayload>(h, g_path);
    if (!p) { refuse(fn, "invalid path", h); return nullptr; }
    if (need_mutable && !p->mutable_path) { refuse(fn, "the path is immutable (not a CGMutablePathRef)", h); return nullptr; }
    return p;
}
ColorPayload* use_color(Addr h, const char* fn) {
    ColorPayload* c = typed<ColorPayload>(h, g_color);
    if (!c) refuse(fn, "invalid color", h);
    return c;
}
bool read_transform(Addr p, Affine& out) {                // `const CGAffineTransform *m`: NULL means identity
    out = Affine{};
    if (!p) return true;
    if (!rt().readable(p, 48)) return false;
    const double* d = reinterpret_cast<const double*>(p);
    out = Affine{d[0], d[1], d[2], d[3], d[4], d[5]};
    return true;
}

void ret_bool(GuestRegs* r, bool b) { r->ret_rax = b ? 1 : 0; }

cg::Color color_of(const ColorPayload& c) { return cg::Color{c.comps[0], c.comps[1], c.comps[2], c.comps[3]}; }

// ---- CGColorSpace ---------------------------------------------------------------------------------------------------------------------------------
Addr w_CGColorSpaceCreateDeviceRGB() { return make(g_space, std::make_unique<SpacePayload>()); }          // +1 (the "Create" rule)
Addr w_CGColorSpaceRetain(Addr s) { if (!typed<SpacePayload>(s, g_space)) { if (s) refuse("CGColorSpaceRetain", "invalid color space", s); return 0; } return rt().retain(s); }
void w_CGColorSpaceRelease(Addr s) { if (!s) return; if (!typed<SpacePayload>(s, g_space)) { refuse("CGColorSpaceRelease", "invalid color space", s); return; } rt().release(s); }
uint64_t w_CGColorSpaceGetNumberOfComponents(Addr s) { return typed<SpacePayload>(s, g_space) ? 3 : 0; }

// ---- CGColor --------------------------------------------------------------------------------------------------------------------------------------
Addr new_color(double r, double g, double b, double a) {
    auto p = std::make_unique<ColorPayload>();
    p->comps[0] = r; p->comps[1] = g; p->comps[2] = b; p->comps[3] = a;
    return make(g_color, std::move(p));
}
Addr w_CGColorCreate(Addr space, uint64_t comps) {
    if (!typed<SpacePayload>(space, g_space)) { refuse("CGColorCreate", "invalid color space", space); return 0; }
    if (!comps || !rt().readable(comps, 4 * sizeof(double))) { refuse("CGColorCreate", "components is not readable memory", comps); return 0; }
    const double* d = reinterpret_cast<const double*>(comps);
    return new_color(d[0], d[1], d[2], d[3]);
}
void w_CGColorCreateGenericRGB(GuestRegs* r) {
    Args a(r);
    const double red = a.next_double(), green = a.next_double(), blue = a.next_double(), alpha = a.next_double();
    r->ret_rax = new_color(red, green, blue, alpha);
}
Addr w_CGColorCreateCopy(Addr c) {
    ColorPayload* p = use_color(c, "CGColorCreateCopy");
    return p ? new_color(p->comps[0], p->comps[1], p->comps[2], p->comps[3]) : 0;
}
Addr w_CGColorRetain(Addr c) { if (!use_color(c, "CGColorRetain")) return 0; return rt().retain(c); }
void w_CGColorRelease(Addr c) { if (!c) return; if (!use_color(c, "CGColorRelease")) return; rt().release(c); }
uint64_t w_CGColorGetNumberOfComponents(Addr c) { return use_color(c, "CGColorGetNumberOfComponents") ? 4 : 0; }
Addr w_CGColorGetComponents(Addr c) { ColorPayload* p = use_color(c, "CGColorGetComponents"); return p ? reinterpret_cast<Addr>(p->comps) : 0; }
void w_CGColorGetAlpha(GuestRegs* r) { ColorPayload* p = use_color(r->gp[0], "CGColorGetAlpha"); r->ret_xmm0 = sysv::as_bits(p ? p->comps[3] : 0.0); }

// ---- CGPath ---------------------------------------------------------------------------------------------------------------------------------------
Addr w_CGPathCreateMutable() {
    auto p = std::make_unique<PathPayload>();
    p->mutable_path = true;
    return make(g_path, std::move(p));
}
Addr copy_path(Addr h, bool mut, const char* fn) {
    PathPayload* src = use_path(h, fn, false);
    if (!src) return 0;
    auto p = std::make_unique<PathPayload>();
    p->path = src->path;
    p->mutable_path = mut;
    return make(g_path, std::move(p));
}
Addr w_CGPathCreateCopy(Addr h) { return copy_path(h, false, "CGPathCreateCopy"); }
Addr w_CGPathCreateMutableCopy(Addr h) { return copy_path(h, true, "CGPathCreateMutableCopy"); }
Addr w_CGPathRetain(Addr h) { if (!use_path(h, "CGPathRetain", false)) return 0; return rt().retain(h); }
void w_CGPathRelease(Addr h) { if (!h) return; if (!use_path(h, "CGPathRelease", false)) return; rt().release(h); }

void w_CGPathMoveToPoint(GuestRegs* r) {
    Args a(r);
    const Addr h = a.next_int(), m = a.next_int();
    const double x = a.next_double(), y = a.next_double();
    PathPayload* p = use_path(h, "CGPathMoveToPoint", true);
    Affine t;
    if (!p || !read_transform(m, t)) { if (p) refuse("CGPathMoveToPoint", "transform is not readable memory", m); return; }
    p->path.move_to(t.apply(Point{x, y}));
}
void w_CGPathAddLineToPoint(GuestRegs* r) {
    Args a(r);
    const Addr h = a.next_int(), m = a.next_int();
    const double x = a.next_double(), y = a.next_double();
    PathPayload* p = use_path(h, "CGPathAddLineToPoint", true);
    Affine t;
    if (!p || !read_transform(m, t)) { if (p) refuse("CGPathAddLineToPoint", "transform is not readable memory", m); return; }
    p->path.line_to(t.apply(Point{x, y}));
}
void w_CGPathAddCurveToPoint(GuestRegs* r) {
    Args a(r);
    const Addr h = a.next_int(), m = a.next_int();
    const double c1x = a.next_double(), c1y = a.next_double(), c2x = a.next_double(), c2y = a.next_double(), x = a.next_double(), y = a.next_double();
    PathPayload* p = use_path(h, "CGPathAddCurveToPoint", true);
    Affine t;
    if (!p || !read_transform(m, t)) { if (p) refuse("CGPathAddCurveToPoint", "transform is not readable memory", m); return; }
    p->path.curve_to(t.apply(Point{c1x, c1y}), t.apply(Point{c2x, c2y}), t.apply(Point{x, y}));
}
void w_CGPathAddQuadCurveToPoint(GuestRegs* r) {
    Args a(r);
    const Addr h = a.next_int(), m = a.next_int();
    const double cx = a.next_double(), cy = a.next_double(), x = a.next_double(), y = a.next_double();
    PathPayload* p = use_path(h, "CGPathAddQuadCurveToPoint", true);
    Affine t;
    if (!p || !read_transform(m, t)) { if (p) refuse("CGPathAddQuadCurveToPoint", "transform is not readable memory", m); return; }
    p->path.quad_to(t.apply(Point{cx, cy}), t.apply(Point{x, y}));
}
void w_CGPathAddRect(GuestRegs* r) {
    Args a(r);
    const Addr h = a.next_int(), m = a.next_int();
    const Rect rc = a.next_rect();
    PathPayload* p = use_path(h, "CGPathAddRect", true);
    Affine t;
    if (!p || !read_transform(m, t)) { if (p) refuse("CGPathAddRect", "transform is not readable memory", m); return; }
    cg::Path tmp;
    tmp.add_rect(rc);
    p->path.add_path(tmp, t);
}
void w_CGPathAddEllipseInRect(GuestRegs* r) {
    Args a(r);
    const Addr h = a.next_int(), m = a.next_int();
    const Rect rc = a.next_rect();
    PathPayload* p = use_path(h, "CGPathAddEllipseInRect", true);
    Affine t;
    if (!p || !read_transform(m, t)) { if (p) refuse("CGPathAddEllipseInRect", "transform is not readable memory", m); return; }
    cg::Path tmp;
    tmp.add_ellipse(rc);
    p->path.add_path(tmp, t);
}
void w_CGPathAddArc(GuestRegs* r) {                                    // (path, m, x, y, radius, startAngle, endAngle, clockwise)
    Args a(r);
    const Addr h = a.next_int(), m = a.next_int();
    const int clockwise = static_cast<int>(a.next_int() & 0xff);
    const double x = a.next_double(), y = a.next_double(), radius = a.next_double(), start = a.next_double(), end = a.next_double();
    PathPayload* p = use_path(h, "CGPathAddArc", true);
    Affine t;
    if (!p || !read_transform(m, t)) { if (p) refuse("CGPathAddArc", "transform is not readable memory", m); return; }
    cg::Path tmp;
    tmp.arc(Point{x, y}, radius, start, end, clockwise != 0);
    p->path.add_path(tmp, t, true);
}
void w_CGPathCloseSubpath(GuestRegs* r) {
    PathPayload* p = use_path(r->gp[0], "CGPathCloseSubpath", true);
    if (p) p->path.close();
}
void w_CGPathAddPath(GuestRegs* r) {
    Args a(r);
    const Addr h = a.next_int(), m = a.next_int(), other = a.next_int();
    PathPayload* p = use_path(h, "CGPathAddPath", true);
    PathPayload* o = use_path(other, "CGPathAddPath", false);
    Affine t;
    if (!p || !o || !read_transform(m, t)) return;
    const cg::Path copy = o->path;                                      // h may equal other
    p->path.add_path(copy, t);
}
void w_CGPathIsEmpty(GuestRegs* r) { PathPayload* p = use_path(r->gp[0], "CGPathIsEmpty", false); ret_bool(r, !p || p->path.empty()); }
void w_CGPathGetBoundingBox(GuestRegs* r) {                            // hidden pointer in rdi, path in rsi: bounding box including control points
    Args a(r);
    const uint64_t out = a.next_int();
    const Addr h = a.next_int();
    PathPayload* p = use_path(h, "CGPathGetBoundingBox", false);
    const Rect box = p ? p->path.bounding_box() : geometry::null_rect();
    sysv::ret_rect(r, out, p && !p->path.empty() ? box : geometry::null_rect());
}
void w_CGPathGetCurrentPoint(GuestRegs* r) {
    PathPayload* p = use_path(r->gp[0], "CGPathGetCurrentPoint", false);
    sysv::ret_point(r, p && p->path.has_current_point() ? p->path.current_point() : Point{0, 0});
}
void w_CGPathContainsPoint(GuestRegs* r) {                             // (path, m, point, eoFill)
    Args a(r);
    const Addr h = a.next_int(), m = a.next_int();
    const bool eo = (a.next_int() & 0xff) != 0;
    const Point pt = a.next_point();
    PathPayload* p = use_path(h, "CGPathContainsPoint", false);
    Affine t;
    if (!p || !read_transform(m, t)) { ret_bool(r, false); return; }
    cg::Path tr;
    tr.add_path(p->path, t);
    ret_bool(r, tr.contains(pt, eo ? cg::FillRule::EvenOdd : cg::FillRule::NonZero));
}
void w_CGPathCreateWithRect(GuestRegs* r) {                            // (rect on the stack, m)
    Args a(r);
    const Addr m = a.next_int();
    const Rect rc = a.next_rect();
    Affine t;
    if (!read_transform(m, t)) { refuse("CGPathCreateWithRect", "transform is not readable memory", m); return; }
    auto p = std::make_unique<PathPayload>();
    cg::Path tmp;
    tmp.add_rect(rc);
    p->path.add_path(tmp, t);
    r->ret_rax = make(g_path, std::move(p));
}
void w_CGPathCreateWithEllipseInRect(GuestRegs* r) {
    Args a(r);
    const Addr m = a.next_int();
    const Rect rc = a.next_rect();
    Affine t;
    if (!read_transform(m, t)) { refuse("CGPathCreateWithEllipseInRect", "transform is not readable memory", m); return; }
    auto p = std::make_unique<PathPayload>();
    cg::Path tmp;
    tmp.add_ellipse(rc);
    p->path.add_path(tmp, t);
    r->ret_rax = make(g_path, std::move(p));
}

// ---- CGImage --------------------------------------------------------------------------------------------------------------------------------------
ImagePayload* use_image(Addr h, const char* fn) {
    ImagePayload* p = typed<ImagePayload>(h, g_image);
    if (!p) refuse(fn, "invalid image", h);
    return p;
}
uint64_t w_CGImageGetWidth(Addr h) { ImagePayload* p = use_image(h, "CGImageGetWidth"); return p ? p->image.width : 0; }
uint64_t w_CGImageGetHeight(Addr h) { ImagePayload* p = use_image(h, "CGImageGetHeight"); return p ? p->image.height : 0; }
Addr w_CGImageRetain(Addr h) { if (!use_image(h, "CGImageRetain")) return 0; return rt().retain(h); }
void w_CGImageRelease(Addr h) { if (!h) return; if (!use_image(h, "CGImageRelease")) return; rt().release(h); }

// ---- CGAffineTransform ----------------------------------------------------------------------------------------------------------------------------
// All of them return a 48-byte struct: hidden result pointer in rdi, real arguments after it.
void w_CGAffineTransformMake(GuestRegs* r) {
    Args a(r);
    const uint64_t out = a.next_int();
    Affine t;
    t.a = a.next_double(); t.b = a.next_double(); t.c = a.next_double(); t.d = a.next_double(); t.tx = a.next_double(); t.ty = a.next_double();
    sysv::ret_affine(r, out, t);
}
void w_CGAffineTransformMakeTranslation(GuestRegs* r) { Args a(r); const uint64_t out = a.next_int(); const double x = a.next_double(), y = a.next_double(); sysv::ret_affine(r, out, Affine::translation(x, y)); }
void w_CGAffineTransformMakeScale(GuestRegs* r) { Args a(r); const uint64_t out = a.next_int(); const double x = a.next_double(), y = a.next_double(); sysv::ret_affine(r, out, Affine::scaling(x, y)); }
void w_CGAffineTransformMakeRotation(GuestRegs* r) { Args a(r); const uint64_t out = a.next_int(); sysv::ret_affine(r, out, Affine::rotation(a.next_double())); }
void w_CGAffineTransformConcat(GuestRegs* r) { Args a(r); const uint64_t out = a.next_int(); const Affine t1 = a.next_affine(), t2 = a.next_affine(); sysv::ret_affine(r, out, cg::concat(t1, t2)); }
void w_CGAffineTransformTranslate(GuestRegs* r) { Args a(r); const uint64_t out = a.next_int(); const Affine t = a.next_affine(); const double x = a.next_double(), y = a.next_double(); sysv::ret_affine(r, out, cg::concat(Affine::translation(x, y), t)); }
void w_CGAffineTransformScale(GuestRegs* r) { Args a(r); const uint64_t out = a.next_int(); const Affine t = a.next_affine(); const double x = a.next_double(), y = a.next_double(); sysv::ret_affine(r, out, cg::concat(Affine::scaling(x, y), t)); }
void w_CGAffineTransformRotate(GuestRegs* r) { Args a(r); const uint64_t out = a.next_int(); const Affine t = a.next_affine(); const double ang = a.next_double(); sysv::ret_affine(r, out, cg::concat(Affine::rotation(ang), t)); }
void w_CGAffineTransformInvert(GuestRegs* r) {
    Args a(r);
    const uint64_t out = a.next_int();
    const Affine t = a.next_affine();
    Affine inv;
    sysv::ret_affine(r, out, cg::invert(t, inv) ? inv : t);              // a singular matrix is returned unchanged, like CoreGraphics
}
void w_CGAffineTransformIsIdentity(GuestRegs* r) { Args a(r); ret_bool(r, a.next_affine().is_identity()); }
void w_CGAffineTransformEqualToTransform(GuestRegs* r) {
    Args a(r);
    const Affine p = a.next_affine(), q = a.next_affine();
    ret_bool(r, p.a == q.a && p.b == q.b && p.c == q.c && p.d == q.d && p.tx == q.tx && p.ty == q.ty);
}
void w_CGPointApplyAffineTransform(GuestRegs* r) { Args a(r); const Point p = a.next_point(); const Affine t = a.next_affine(); sysv::ret_point(r, t.apply(p)); }
void w_CGSizeApplyAffineTransform(GuestRegs* r) { Args a(r); const geometry::Size s = a.next_size(); const Affine t = a.next_affine(); sysv::ret_size(r, t.apply(s)); }
void w_CGRectApplyAffineTransform(GuestRegs* r) {
    Args a(r);
    const uint64_t out = a.next_int();
    const Rect rc = a.next_rect();
    const Affine t = a.next_affine();
    sysv::ret_rect(r, out, geometry::is_null(rc) ? rc : cg::transformed_bounds(rc, t));
}

// ---- CGContext ------------------------------------------------------------------------------------------------------------------------------------
#define WITH_CTX(name) Args a(r); const Addr h = a.next_int(); cg::Context* c = use_ctx(h, name); (void)h; if (!c) return;

void w_CGContextRetain(GuestRegs* r) { ContextPayload* c = typed<ContextPayload>(r->gp[0], g_context); if (!c) { if (r->gp[0]) refuse("CGContextRetain", "invalid context", r->gp[0]); r->ret_rax = 0; return; } r->ret_rax = rt().retain(r->gp[0]); }
void w_CGContextRelease(GuestRegs* r) { if (!r->gp[0]) return; if (!typed<ContextPayload>(r->gp[0], g_context)) { refuse("CGContextRelease", "invalid context", r->gp[0]); return; } rt().release(r->gp[0]); }
void w_CGContextSaveGState(GuestRegs* r) { WITH_CTX("CGContextSaveGState") c->save(); }
void w_CGContextRestoreGState(GuestRegs* r) {
    WITH_CTX("CGContextRestoreGState")
    if (!c->restore()) refuse("CGContextRestoreGState", "unbalanced restore (no saved graphics state)", h);
}
void w_CGContextSetFillColorWithColor(GuestRegs* r) {
    WITH_CTX("CGContextSetFillColorWithColor")
    ColorPayload* col = use_color(a.next_int(), "CGContextSetFillColorWithColor");
    if (col) c->mutable_state().fill = color_of(*col);
}
void w_CGContextSetStrokeColorWithColor(GuestRegs* r) {
    WITH_CTX("CGContextSetStrokeColorWithColor")
    ColorPayload* col = use_color(a.next_int(), "CGContextSetStrokeColorWithColor");
    if (col) c->mutable_state().stroke = color_of(*col);
}
void w_CGContextSetRGBFillColor(GuestRegs* r) {
    WITH_CTX("CGContextSetRGBFillColor")
    const double red = a.next_double(), green = a.next_double(), blue = a.next_double(), alpha = a.next_double();
    c->mutable_state().fill = cg::Color{red, green, blue, alpha};
}
void w_CGContextSetRGBStrokeColor(GuestRegs* r) {
    WITH_CTX("CGContextSetRGBStrokeColor")
    const double red = a.next_double(), green = a.next_double(), blue = a.next_double(), alpha = a.next_double();
    c->mutable_state().stroke = cg::Color{red, green, blue, alpha};
}
void w_CGContextSetGrayFillColor(GuestRegs* r) { WITH_CTX("CGContextSetGrayFillColor") const double g = a.next_double(), al = a.next_double(); c->mutable_state().fill = cg::Color{g, g, g, al}; }
void w_CGContextSetGrayStrokeColor(GuestRegs* r) { WITH_CTX("CGContextSetGrayStrokeColor") const double g = a.next_double(), al = a.next_double(); c->mutable_state().stroke = cg::Color{g, g, g, al}; }
void w_CGContextSetAlpha(GuestRegs* r) { WITH_CTX("CGContextSetAlpha") const double al = a.next_double(); c->mutable_state().alpha = al < 0 ? 0 : al > 1 ? 1 : al; }
void w_CGContextSetLineWidth(GuestRegs* r) { WITH_CTX("CGContextSetLineWidth") const double w = a.next_double(); c->mutable_state().line.width = w < 0 ? 0 : w; }
void w_CGContextSetLineCap(GuestRegs* r) { WITH_CTX("CGContextSetLineCap") const int64_t v = static_cast<int32_t>(a.next_int()); if (v >= 0 && v <= 2) c->mutable_state().line.cap = static_cast<cg::LineCap>(v); }
void w_CGContextSetLineJoin(GuestRegs* r) { WITH_CTX("CGContextSetLineJoin") const int64_t v = static_cast<int32_t>(a.next_int()); if (v >= 0 && v <= 2) c->mutable_state().line.join = static_cast<cg::LineJoin>(v); }
void w_CGContextSetMiterLimit(GuestRegs* r) { WITH_CTX("CGContextSetMiterLimit") const double v = a.next_double(); c->mutable_state().line.miter_limit = v < 1 ? 1 : v; }
void w_CGContextSetLineDash(GuestRegs* r) {                                // (ctx, phase, const CGFloat *lengths, size_t count)
    WITH_CTX("CGContextSetLineDash")
    const uint64_t lengths = a.next_int(), count = a.next_int();
    const double phase = a.next_double();
    auto& line = c->mutable_state().line;
    line.dash.clear();
    line.dash_phase = phase;
    if (!lengths || count == 0) return;                                   // a solid line
    if (count > 64 || !rt().readable(lengths, count * sizeof(double))) { refuse("CGContextSetLineDash", "invalid dash array", lengths); return; }
    const double* d = reinterpret_cast<const double*>(lengths);
    double sum = 0;
    for (uint64_t i = 0; i < count; ++i) { if (d[i] < 0) return; sum += d[i]; }
    if (sum <= 0) return;
    line.dash.assign(d, d + count);
    if (count & 1) line.dash.insert(line.dash.end(), d, d + count);        // an odd pattern repeats with swapped roles, as in PDF / CoreGraphics
}
void w_CGContextSetShouldAntialias(GuestRegs* r) { WITH_CTX("CGContextSetShouldAntialias") c->mutable_state().antialias = (a.next_int() & 0xff) != 0; }
void w_CGContextSetInterpolationQuality(GuestRegs* r) { WITH_CTX("CGContextSetInterpolationQuality") c->mutable_state().interpolate = static_cast<int32_t>(a.next_int()) != 1; }   // kCGInterpolationNone = 1

void w_CGContextFillRect(GuestRegs* r) { WITH_CTX("CGContextFillRect") c->fill_rect(a.next_rect()); }
void w_CGContextStrokeRect(GuestRegs* r) { WITH_CTX("CGContextStrokeRect") c->stroke_rect(a.next_rect()); }
void w_CGContextStrokeRectWithWidth(GuestRegs* r) {
    WITH_CTX("CGContextStrokeRectWithWidth")
    const Rect rc = a.next_rect();
    const double w = a.next_double();
    const double saved = c->state().line.width;
    c->mutable_state().line.width = w < 0 ? 0 : w;
    c->stroke_rect(rc);
    c->mutable_state().line.width = saved;
}
void w_CGContextClearRect(GuestRegs* r) { WITH_CTX("CGContextClearRect") c->clear_rect(a.next_rect()); }
void w_CGContextFillEllipseInRect(GuestRegs* r) {
    WITH_CTX("CGContextFillEllipseInRect")
    const Rect rc = a.next_rect();
    cg::Path e;
    e.add_ellipse(rc);
    c->begin_path();
    c->add_path(e);
    c->fill_current(cg::FillRule::NonZero);
}
void w_CGContextStrokeEllipseInRect(GuestRegs* r) {
    WITH_CTX("CGContextStrokeEllipseInRect")
    const Rect rc = a.next_rect();
    cg::Path e;
    e.add_ellipse(rc);
    c->begin_path();
    c->add_path(e);
    c->stroke_current();
}

void w_CGContextBeginPath(GuestRegs* r) { WITH_CTX("CGContextBeginPath") c->begin_path(); }
void w_CGContextClosePath(GuestRegs* r) { WITH_CTX("CGContextClosePath") c->path().close(); }
void w_CGContextMoveToPoint(GuestRegs* r) { WITH_CTX("CGContextMoveToPoint") const double x = a.next_double(), y = a.next_double(); c->path().move_to(c->to_device_space_point(Point{x, y})); }
void w_CGContextAddLineToPoint(GuestRegs* r) { WITH_CTX("CGContextAddLineToPoint") const double x = a.next_double(), y = a.next_double(); c->path().line_to(c->to_device_space_point(Point{x, y})); }
void w_CGContextAddCurveToPoint(GuestRegs* r) {
    WITH_CTX("CGContextAddCurveToPoint")
    const double c1x = a.next_double(), c1y = a.next_double(), c2x = a.next_double(), c2y = a.next_double(), x = a.next_double(), y = a.next_double();
    c->path().curve_to(c->to_device_space_point(Point{c1x, c1y}), c->to_device_space_point(Point{c2x, c2y}), c->to_device_space_point(Point{x, y}));
}
void w_CGContextAddQuadCurveToPoint(GuestRegs* r) {
    WITH_CTX("CGContextAddQuadCurveToPoint")
    const double cx = a.next_double(), cy = a.next_double(), x = a.next_double(), y = a.next_double();
    c->path().quad_to(c->to_device_space_point(Point{cx, cy}), c->to_device_space_point(Point{x, y}));
}
void w_CGContextAddRect(GuestRegs* r) { WITH_CTX("CGContextAddRect") cg::Path p; p.add_rect(a.next_rect()); c->add_path(p); }
void w_CGContextAddRects(GuestRegs* r) {
    WITH_CTX("CGContextAddRects")
    const uint64_t rects = a.next_int(), count = a.next_int();
    if (!rects || count == 0) return;
    if (count > (1u << 20) || !rt().readable(rects, count * 32)) { refuse("CGContextAddRects", "invalid rectangle array", rects); return; }
    const double* d = reinterpret_cast<const double*>(rects);
    for (uint64_t i = 0; i < count; ++i) { cg::Path p; p.add_rect(geometry::make_rect(d[i * 4], d[i * 4 + 1], d[i * 4 + 2], d[i * 4 + 3])); c->add_path(p); }
}
void w_CGContextAddEllipseInRect(GuestRegs* r) { WITH_CTX("CGContextAddEllipseInRect") cg::Path p; p.add_ellipse(a.next_rect()); c->add_path(p); }
void w_CGContextAddArc(GuestRegs* r) {                                     // (ctx, x, y, radius, startAngle, endAngle, clockwise)
    WITH_CTX("CGContextAddArc")
    const int clockwise = static_cast<int>(a.next_int() & 0xff);
    const double x = a.next_double(), y = a.next_double(), radius = a.next_double(), start = a.next_double(), end = a.next_double();
    cg::Path p;
    p.arc(Point{x, y}, radius, start, end, clockwise != 0);
    c->path().add_path(p, c->state().ctm, true);
}
void w_CGContextAddArcToPoint(GuestRegs* r) {                              // (ctx, x1, y1, x2, y2, radius)
    WITH_CTX("CGContextAddArcToPoint")
    const double x1 = a.next_double(), y1 = a.next_double(), x2 = a.next_double(), y2 = a.next_double(), radius = a.next_double();
    cg::Path p;
    if (c->path().has_current_point()) {
        Affine inv;
        if (!cg::invert(c->state().ctm, inv)) return;
        p.move_to(inv.apply(c->path().current_point()));                    // the current point back in user space
    }
    p.arc_to(Point{x1, y1}, Point{x2, y2}, radius);
    c->path().add_path(p, c->state().ctm, true);
}
void w_CGContextAddPath(GuestRegs* r) {
    WITH_CTX("CGContextAddPath")
    PathPayload* p = use_path(a.next_int(), "CGContextAddPath", false);
    if (p) c->add_path(p->path);
}
void w_CGContextStrokePath(GuestRegs* r) { WITH_CTX("CGContextStrokePath") c->stroke_current(); }
void w_CGContextFillPath(GuestRegs* r) { WITH_CTX("CGContextFillPath") c->fill_current(cg::FillRule::NonZero); }
void w_CGContextEOFillPath(GuestRegs* r) { WITH_CTX("CGContextEOFillPath") c->fill_current(cg::FillRule::EvenOdd); }
void w_CGContextDrawPath(GuestRegs* r) {
    WITH_CTX("CGContextDrawPath")
    const int32_t mode = static_cast<int32_t>(a.next_int());
    if (mode < 0 || mode > 4) { refuse("CGContextDrawPath", "invalid drawing mode", static_cast<Addr>(mode)); return; }
    c->draw_current(static_cast<cg::PathMode>(mode));
}
void w_CGContextClip(GuestRegs* r) { WITH_CTX("CGContextClip") c->clip_current(cg::FillRule::NonZero); }
void w_CGContextEOClip(GuestRegs* r) { WITH_CTX("CGContextEOClip") c->clip_current(cg::FillRule::EvenOdd); }
void w_CGContextClipToRect(GuestRegs* r) { WITH_CTX("CGContextClipToRect") c->clip_to_rect(a.next_rect()); }
void w_CGContextTranslateCTM(GuestRegs* r) { WITH_CTX("CGContextTranslateCTM") const double x = a.next_double(), y = a.next_double(); c->translate(x, y); }
void w_CGContextScaleCTM(GuestRegs* r) { WITH_CTX("CGContextScaleCTM") const double x = a.next_double(), y = a.next_double(); c->scale(x, y); }
void w_CGContextRotateCTM(GuestRegs* r) { WITH_CTX("CGContextRotateCTM") c->rotate(a.next_double()); }
void w_CGContextConcatCTM(GuestRegs* r) { WITH_CTX("CGContextConcatCTM") c->concat_ctm(a.next_affine()); }
void w_CGContextGetCTM(GuestRegs* r) {                                     // hidden pointer in rdi, context in rsi
    Args a(r);
    const uint64_t out = a.next_int();
    const Addr h = a.next_int();
    cg::Context* c = use_ctx(h, "CGContextGetCTM");
    sysv::ret_affine(r, out, c ? c->state().ctm : Affine{});
}
void w_CGContextDrawImage(GuestRegs* r) {                                  // (ctx, CGRect rect, CGImageRef)
    WITH_CTX("CGContextDrawImage")
    const Rect rc = a.next_rect();
    ImagePayload* img = use_image(a.next_int(), "CGContextDrawImage");
    if (img) c->draw_image(img->image, rc);
}
void w_CGContextFlush(GuestRegs* r) { WITH_CTX("CGContextFlush") (void)c; }          // window contexts present when the drawing pass ends: nothing to flush
void w_CGContextSynchronize(GuestRegs* r) { WITH_CTX("CGContextSynchronize") (void)c; }

}  // namespace

// ---- internal API (appkit) ----------------------------------------------------------------------------------------------------------------------
Addr cg_context_create(cg::Device* device, const Affine& base, geometry::Size size) {
    auto p = std::make_unique<ContextPayload>();
    p->device = device;
    p->engine = std::make_unique<cg::Context>(device, base, size);
    return make(g_context, std::move(p));
}
cg::Context* cg_context_engine(Addr ctx) {
    ContextPayload* c = typed<ContextPayload>(ctx, g_context);
    return c ? c->engine.get() : nullptr;
}
void cg_context_invalidate(Addr ctx) {
    if (ContextPayload* c = typed<ContextPayload>(ctx, g_context)) { c->engine.reset(); c->device = nullptr; }
}
Addr cg_image_create(const cg::Image& image) {
    if (!image.valid()) return 0;
    auto p = std::make_unique<ImagePayload>();
    p->image = image;
    return make(g_image, std::move(p));
}
const cg::Image* cg_image_pixels(Addr image) {
    ImagePayload* p = typed<ImagePayload>(image, g_image);
    return p ? &p->image : nullptr;
}
uint64_t cg_invalid_handle_events() { return g_invalid.load(); }
size_t cg_live_objects() { const int64_t n = g_live.load(); return n < 0 ? 0 : static_cast<size_t>(n); }

namespace {
Addr dealloc_payload(Addr self, Addr) {
    if (uint64_t* slot = fnd::payload_slot(self)) { fnd::store_drop(*slot); *slot = 0; }
    rt().destruct_instance(self);
    rt().dispose_instance(self);
    return 0;
}
template <class F> void* addr(F* f) { return reinterpret_cast<void*>(f); }
Class* declare(objc::Runtime& r, const char* name) {
    Class* c = r.declare_native_class(name, "NSObject", 16);
    fnd::classes().extra_payload.push_back(c);
    r.add_native_methods(c, false, {{"dealloc", "v16@0:8", addr(dealloc_payload), false}});
    return c;
}
}  // namespace

void install_cg_api(objc::Runtime& r) {
    g_space = declare(r, "__NSCGColorSpace");
    g_color = declare(r, "__NSCGColor");
    g_path = declare(r, "__NSCGPath");
    g_image = declare(r, "__NSCGImage");
    g_context = declare(r, "__NSCGContext");
    g_identity = r.arena_alloc(48, 16);
    double* d = reinterpret_cast<double*>(g_identity);
    d[0] = 1; d[1] = 0; d[2] = 0; d[3] = 1; d[4] = 0; d[5] = 0;
    r.add_dynamic_provider([](std::string_view name) -> std::optional<uint64_t> {
        if (name == "_CGAffineTransformIdentity") return g_identity;
        return std::nullopt;
    });
}

void add_cg_api_symbols(std::vector<libsystem::HostSymbol>& t) {
    using libsystem::HostSymbol;
    auto raw = [&](const char* name, void (*f)(GuestRegs*)) { t.push_back(HostSymbol{name, reinterpret_cast<void*>(f), true, false, false, libsystem::Lib::CoreGraphics}); };
    auto pos = [&](const char* name, void* f) { t.push_back(HostSymbol{name, f, false, false, false, libsystem::Lib::CoreGraphics}); };
    // color spaces and colors
    pos("_CGColorSpaceCreateDeviceRGB", addr(w_CGColorSpaceCreateDeviceRGB));
    pos("_CGColorSpaceRetain", addr(w_CGColorSpaceRetain));
    pos("_CGColorSpaceRelease", addr(w_CGColorSpaceRelease));
    pos("_CGColorSpaceGetNumberOfComponents", addr(w_CGColorSpaceGetNumberOfComponents));
    pos("_CGColorCreate", addr(w_CGColorCreate));
    raw("_CGColorCreateGenericRGB", w_CGColorCreateGenericRGB);
    pos("_CGColorCreateCopy", addr(w_CGColorCreateCopy));
    pos("_CGColorRetain", addr(w_CGColorRetain));
    pos("_CGColorRelease", addr(w_CGColorRelease));
    pos("_CGColorGetNumberOfComponents", addr(w_CGColorGetNumberOfComponents));
    pos("_CGColorGetComponents", addr(w_CGColorGetComponents));
    raw("_CGColorGetAlpha", w_CGColorGetAlpha);
    // paths
    pos("_CGPathCreateMutable", addr(w_CGPathCreateMutable));
    pos("_CGPathCreateCopy", addr(w_CGPathCreateCopy));
    pos("_CGPathCreateMutableCopy", addr(w_CGPathCreateMutableCopy));
    pos("_CGPathRetain", addr(w_CGPathRetain));
    pos("_CGPathRelease", addr(w_CGPathRelease));
    raw("_CGPathMoveToPoint", w_CGPathMoveToPoint);
    raw("_CGPathAddLineToPoint", w_CGPathAddLineToPoint);
    raw("_CGPathAddCurveToPoint", w_CGPathAddCurveToPoint);
    raw("_CGPathAddQuadCurveToPoint", w_CGPathAddQuadCurveToPoint);
    raw("_CGPathAddRect", w_CGPathAddRect);
    raw("_CGPathAddEllipseInRect", w_CGPathAddEllipseInRect);
    raw("_CGPathAddArc", w_CGPathAddArc);
    raw("_CGPathAddPath", w_CGPathAddPath);
    raw("_CGPathCloseSubpath", w_CGPathCloseSubpath);
    raw("_CGPathIsEmpty", w_CGPathIsEmpty);
    raw("_CGPathGetBoundingBox", w_CGPathGetBoundingBox);
    raw("_CGPathGetCurrentPoint", w_CGPathGetCurrentPoint);
    raw("_CGPathContainsPoint", w_CGPathContainsPoint);
    raw("_CGPathCreateWithRect", w_CGPathCreateWithRect);
    raw("_CGPathCreateWithEllipseInRect", w_CGPathCreateWithEllipseInRect);
    // images (no guest-side creation path yet: images come from the AppKit image classes in a later phase)
    pos("_CGImageGetWidth", addr(w_CGImageGetWidth));
    pos("_CGImageGetHeight", addr(w_CGImageGetHeight));
    pos("_CGImageRetain", addr(w_CGImageRetain));
    pos("_CGImageRelease", addr(w_CGImageRelease));
    // transforms
    raw("_CGAffineTransformMake", w_CGAffineTransformMake);
    raw("_CGAffineTransformMakeTranslation", w_CGAffineTransformMakeTranslation);
    raw("_CGAffineTransformMakeScale", w_CGAffineTransformMakeScale);
    raw("_CGAffineTransformMakeRotation", w_CGAffineTransformMakeRotation);
    raw("_CGAffineTransformConcat", w_CGAffineTransformConcat);
    raw("_CGAffineTransformTranslate", w_CGAffineTransformTranslate);
    raw("_CGAffineTransformScale", w_CGAffineTransformScale);
    raw("_CGAffineTransformRotate", w_CGAffineTransformRotate);
    raw("_CGAffineTransformInvert", w_CGAffineTransformInvert);
    raw("_CGAffineTransformIsIdentity", w_CGAffineTransformIsIdentity);
    raw("_CGAffineTransformEqualToTransform", w_CGAffineTransformEqualToTransform);
    raw("_CGPointApplyAffineTransform", w_CGPointApplyAffineTransform);
    raw("_CGSizeApplyAffineTransform", w_CGSizeApplyAffineTransform);
    raw("_CGRectApplyAffineTransform", w_CGRectApplyAffineTransform);
    // contexts
    raw("_CGContextRetain", w_CGContextRetain);
    raw("_CGContextRelease", w_CGContextRelease);
    raw("_CGContextSaveGState", w_CGContextSaveGState);
    raw("_CGContextRestoreGState", w_CGContextRestoreGState);
    raw("_CGContextSetFillColorWithColor", w_CGContextSetFillColorWithColor);
    raw("_CGContextSetStrokeColorWithColor", w_CGContextSetStrokeColorWithColor);
    raw("_CGContextSetRGBFillColor", w_CGContextSetRGBFillColor);
    raw("_CGContextSetRGBStrokeColor", w_CGContextSetRGBStrokeColor);
    raw("_CGContextSetGrayFillColor", w_CGContextSetGrayFillColor);
    raw("_CGContextSetGrayStrokeColor", w_CGContextSetGrayStrokeColor);
    raw("_CGContextSetAlpha", w_CGContextSetAlpha);
    raw("_CGContextSetLineWidth", w_CGContextSetLineWidth);
    raw("_CGContextSetLineCap", w_CGContextSetLineCap);
    raw("_CGContextSetLineJoin", w_CGContextSetLineJoin);
    raw("_CGContextSetMiterLimit", w_CGContextSetMiterLimit);
    raw("_CGContextSetLineDash", w_CGContextSetLineDash);
    raw("_CGContextSetShouldAntialias", w_CGContextSetShouldAntialias);
    raw("_CGContextSetInterpolationQuality", w_CGContextSetInterpolationQuality);
    raw("_CGContextFillRect", w_CGContextFillRect);
    raw("_CGContextStrokeRect", w_CGContextStrokeRect);
    raw("_CGContextStrokeRectWithWidth", w_CGContextStrokeRectWithWidth);
    raw("_CGContextClearRect", w_CGContextClearRect);
    raw("_CGContextFillEllipseInRect", w_CGContextFillEllipseInRect);
    raw("_CGContextStrokeEllipseInRect", w_CGContextStrokeEllipseInRect);
    raw("_CGContextBeginPath", w_CGContextBeginPath);
    raw("_CGContextClosePath", w_CGContextClosePath);
    raw("_CGContextMoveToPoint", w_CGContextMoveToPoint);
    raw("_CGContextAddLineToPoint", w_CGContextAddLineToPoint);
    raw("_CGContextAddCurveToPoint", w_CGContextAddCurveToPoint);
    raw("_CGContextAddQuadCurveToPoint", w_CGContextAddQuadCurveToPoint);
    raw("_CGContextAddRect", w_CGContextAddRect);
    raw("_CGContextAddRects", w_CGContextAddRects);
    raw("_CGContextAddEllipseInRect", w_CGContextAddEllipseInRect);
    raw("_CGContextAddArc", w_CGContextAddArc);
    raw("_CGContextAddArcToPoint", w_CGContextAddArcToPoint);
    raw("_CGContextAddPath", w_CGContextAddPath);
    raw("_CGContextStrokePath", w_CGContextStrokePath);
    raw("_CGContextFillPath", w_CGContextFillPath);
    raw("_CGContextEOFillPath", w_CGContextEOFillPath);
    raw("_CGContextDrawPath", w_CGContextDrawPath);
    raw("_CGContextClip", w_CGContextClip);
    raw("_CGContextEOClip", w_CGContextEOClip);
    raw("_CGContextClipToRect", w_CGContextClipToRect);
    raw("_CGContextTranslateCTM", w_CGContextTranslateCTM);
    raw("_CGContextScaleCTM", w_CGContextScaleCTM);
    raw("_CGContextRotateCTM", w_CGContextRotateCTM);
    raw("_CGContextConcatCTM", w_CGContextConcatCTM);
    raw("_CGContextGetCTM", w_CGContextGetCTM);
    raw("_CGContextDrawImage", w_CGContextDrawImage);
    raw("_CGContextFlush", w_CGContextFlush);
    raw("_CGContextSynchronize", w_CGContextSynchronize);
}

}  // namespace cider::graphics
