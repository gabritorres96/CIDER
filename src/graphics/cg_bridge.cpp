// CoreGraphics geometry and the NSGeometry functions of Foundation (CIDER 0.5.6), implemented on top of cider::geometry.
//
// System V passing of the geometry structures, as clang emits it for x86_64 Darwin:
//   * CGPoint / CGSize (two doubles, 16 bytes, class SSE): two consecutive xmm registers (xmm0:xmm1 for the first one).
//   * CGRect (four doubles, 32 bytes, class MEMORY): copied onto the stack, in argument order, 8-byte aligned.
//   * a CGRect result is returned through a hidden pointer in rdi (the first integer argument), which is also returned in rax.
// The functions are therefore raw handlers that walk the argument registers and the stack area themselves.
// Not implemented: affine transforms, dictionary representations, "nearly equal" comparisons, NSRange helpers, NSDivideRect.
#include <cstring>
#include <format>

#include "cider/abi.hpp"
#include "cider/geometry.hpp"
#include "cider/libsystem.hpp"
#include "cider/objc.hpp"
#include "../foundation/foundation_internal.hpp"
#include "cg_api.hpp"
#include "cg_bridge.hpp"
#include "sysv_args.hpp"
#include "../libsystem/internal.hpp"

namespace cider::graphics {
namespace {

using abi::GuestRegs;
using geometry::Point;
using geometry::Rect;
using geometry::Size;

using sysv::Args;
using sysv::as_double;
using sysv::as_bits;

void put_rect(GuestRegs* r, uint64_t out_ptr, const Rect& v) {
    double* d = reinterpret_cast<double*>(out_ptr);
    d[0] = v.origin.x; d[1] = v.origin.y; d[2] = v.size.width; d[3] = v.size.height;
    r->ret_rax = out_ptr;
}
void ret_double(GuestRegs* r, double d) { r->ret_xmm0 = as_bits(d); }
void ret_bool(GuestRegs* r, bool b) { r->ret_rax = b ? 1 : 0; }

// ---- CGRect accessors -------------------------------------------------------------------------------------------------------------------
#define RECT_GETTER(name, expr) void name(GuestRegs* r) { Args a(r); const Rect rc = a.next_rect(); ret_double(r, expr); }
RECT_GETTER(w_CGRectGetMinX, rc.min_x())
RECT_GETTER(w_CGRectGetMidX, rc.mid_x())
RECT_GETTER(w_CGRectGetMaxX, rc.max_x())
RECT_GETTER(w_CGRectGetMinY, rc.min_y())
RECT_GETTER(w_CGRectGetMidY, rc.mid_y())
RECT_GETTER(w_CGRectGetMaxY, rc.max_y())
RECT_GETTER(w_CGRectGetWidth, rc.width())
RECT_GETTER(w_CGRectGetHeight, rc.height())
#undef RECT_GETTER

void w_CGPointEqualToPoint(GuestRegs* r) { Args a(r); const Point p = a.next_point(), q = a.next_point(); ret_bool(r, geometry::equal(p, q)); }
void w_CGSizeEqualToSize(GuestRegs* r) { Args a(r); const Size p = a.next_size(), q = a.next_size(); ret_bool(r, geometry::equal(p, q)); }
void w_CGRectEqualToRect(GuestRegs* r) { Args a(r); const Rect p = a.next_rect(), q = a.next_rect(); ret_bool(r, geometry::equal(p, q)); }
void w_CGRectIsEmpty(GuestRegs* r) { Args a(r); ret_bool(r, geometry::is_empty(a.next_rect())); }
void w_CGRectIsNull(GuestRegs* r) { Args a(r); ret_bool(r, geometry::is_null(a.next_rect())); }
void w_CGRectIsInfinite(GuestRegs* r) { Args a(r); ret_bool(r, geometry::is_infinite(a.next_rect())); }
void w_CGRectContainsPoint(GuestRegs* r) { Args a(r); const Rect rc = a.next_rect(); const Point p = a.next_point(); ret_bool(r, geometry::contains(rc, p)); }
void w_CGRectContainsRect(GuestRegs* r) { Args a(r); const Rect p = a.next_rect(), q = a.next_rect(); ret_bool(r, geometry::contains(p, q)); }
void w_CGRectIntersectsRect(GuestRegs* r) { Args a(r); const Rect p = a.next_rect(), q = a.next_rect(); ret_bool(r, geometry::intersects(p, q)); }

// rect-returning functions: rdi is the hidden result pointer
void w_CGRectStandardize(GuestRegs* r) { Args a(r); const uint64_t out = a.next_int(); put_rect(r, out, geometry::standardize(a.next_rect())); }
void w_CGRectIntegral(GuestRegs* r) { Args a(r); const uint64_t out = a.next_int(); put_rect(r, out, geometry::integral(a.next_rect())); }
void w_CGRectInset(GuestRegs* r) { Args a(r); const uint64_t out = a.next_int(); const Rect rc = a.next_rect(); const double dx = a.next_double(), dy = a.next_double(); put_rect(r, out, geometry::inset(rc, dx, dy)); }
void w_CGRectOffset(GuestRegs* r) { Args a(r); const uint64_t out = a.next_int(); const Rect rc = a.next_rect(); const double dx = a.next_double(), dy = a.next_double(); put_rect(r, out, geometry::offset(rc, dx, dy)); }
void w_CGRectUnion(GuestRegs* r) { Args a(r); const uint64_t out = a.next_int(); const Rect p = a.next_rect(), q = a.next_rect(); put_rect(r, out, geometry::union_rect(p, q)); }
void w_CGRectIntersection(GuestRegs* r) { Args a(r); const uint64_t out = a.next_int(); const Rect p = a.next_rect(), q = a.next_rect(); put_rect(r, out, geometry::intersection(p, q)); }

// void CGRectDivide(CGRect rect, CGRect* slice, CGRect* remainder, CGFloat amount, CGRectEdge edge)   edge: MinX=0 MinY=1 MaxX=2 MaxY=3
void w_CGRectDivide(GuestRegs* r) {
    Args a(r);
    const Rect rc = geometry::standardize(a.next_rect());
    const uint64_t slice = a.next_int(), remainder = a.next_int();
    const double amount = a.next_double();
    const int edge = static_cast<int>(a.next_int());
    auto* c = libsystem::context();
    if (!c || !c->objc || !c->objc->readable(slice, 32) || !c->objc->readable(remainder, 32)) { libsystem::detail::trap("CGRectDivide: slice/remainder pointers are not writable"); return; }
    Rect s = rc, rem = rc;
    const double amt = std::max(0.0, std::min(amount, (edge == 0 || edge == 2) ? rc.size.width : rc.size.height));
    switch (edge) {
        case 0: s.size.width = amt; rem.origin.x += amt; rem.size.width -= amt; break;
        case 1: s.size.height = amt; rem.origin.y += amt; rem.size.height -= amt; break;
        case 2: s.origin.x = rc.max_x() - amt; s.size.width = amt; rem.size.width -= amt; break;
        default: s.origin.y = rc.max_y() - amt; s.size.height = amt; rem.size.height -= amt; break;
    }
    put_rect(r, slice, s);
    put_rect(r, remainder, rem);
}

// ---- NSGeometry (64-bit: NSRect == CGRect) ---------------------------------------------------------------------------------------------------
void w_NSPointInRect(GuestRegs* r) { Args a(r); const Point p = a.next_point(); const Rect rc = a.next_rect(); ret_bool(r, geometry::contains(rc, p)); }
void w_NSMouseInRect(GuestRegs* r) {                                 // point, rect, flipped: for unflipped rects the top edge is inclusive and the bottom exclusive
    Args a(r);
    const Point p = a.next_point();
    const Rect rc = geometry::standardize(a.next_rect());
    const bool flipped = (a.next_int() & 0xff) != 0;
    const bool in_x = p.x >= rc.min_x() && p.x < rc.max_x();
    const bool in_y = flipped ? (p.y >= rc.min_y() && p.y < rc.max_y()) : (p.y > rc.min_y() && p.y <= rc.max_y());
    ret_bool(r, in_x && in_y);
}
void w_NSContainsRect(GuestRegs* r) { w_CGRectContainsRect(r); }
void w_NSIsEmptyRect(GuestRegs* r) { w_CGRectIsEmpty(r); }

std::string fmt(double v) { return std::format("{:g}", v); }
uint64_t new_str(const std::string& s) { return foundation::new_string_autoreleased(s); }
void w_NSStringFromRect(GuestRegs* r) { Args a(r); const Rect rc = a.next_rect(); r->ret_rax = new_str("{{" + fmt(rc.origin.x) + ", " + fmt(rc.origin.y) + "}, {" + fmt(rc.size.width) + ", " + fmt(rc.size.height) + "}}"); }
void w_NSStringFromPoint(GuestRegs* r) { Args a(r); const Point p = a.next_point(); r->ret_rax = new_str("{" + fmt(p.x) + ", " + fmt(p.y) + "}"); }
void w_NSStringFromSize(GuestRegs* r) { Args a(r); const Size s = a.next_size(); r->ret_rax = new_str("{" + fmt(s.width) + ", " + fmt(s.height) + "}"); }

}  // namespace

void add_cg_symbols(std::vector<libsystem::HostSymbol>& t) {
    using libsystem::HostSymbol;
    using libsystem::Lib;
    auto fn = [&](const char* name, void (*f)(GuestRegs*), Lib lib) { t.push_back(HostSymbol{name, reinterpret_cast<void*>(f), true, false, false, lib}); };
    const Lib cg = Lib::CoreGraphics, fnd = Lib::Foundation;
    fn("_CGRectGetMinX", w_CGRectGetMinX, cg);
    fn("_CGRectGetMidX", w_CGRectGetMidX, cg);
    fn("_CGRectGetMaxX", w_CGRectGetMaxX, cg);
    fn("_CGRectGetMinY", w_CGRectGetMinY, cg);
    fn("_CGRectGetMidY", w_CGRectGetMidY, cg);
    fn("_CGRectGetMaxY", w_CGRectGetMaxY, cg);
    fn("_CGRectGetWidth", w_CGRectGetWidth, cg);
    fn("_CGRectGetHeight", w_CGRectGetHeight, cg);
    fn("_CGPointEqualToPoint", w_CGPointEqualToPoint, cg);
    fn("_CGSizeEqualToSize", w_CGSizeEqualToSize, cg);
    fn("_CGRectEqualToRect", w_CGRectEqualToRect, cg);
    fn("_CGRectIsEmpty", w_CGRectIsEmpty, cg);
    fn("_CGRectIsNull", w_CGRectIsNull, cg);
    fn("_CGRectIsInfinite", w_CGRectIsInfinite, cg);
    fn("_CGRectContainsPoint", w_CGRectContainsPoint, cg);
    fn("_CGRectContainsRect", w_CGRectContainsRect, cg);
    fn("_CGRectIntersectsRect", w_CGRectIntersectsRect, cg);
    fn("_CGRectStandardize", w_CGRectStandardize, cg);
    fn("_CGRectIntegral", w_CGRectIntegral, cg);
    fn("_CGRectInset", w_CGRectInset, cg);
    fn("_CGRectOffset", w_CGRectOffset, cg);
    fn("_CGRectUnion", w_CGRectUnion, cg);
    fn("_CGRectIntersection", w_CGRectIntersection, cg);
    fn("_CGRectDivide", w_CGRectDivide, cg);
    fn("_NSPointInRect", w_NSPointInRect, fnd);
    fn("_NSMouseInRect", w_NSMouseInRect, fnd);
    fn("_NSContainsRect", w_NSContainsRect, fnd);
    fn("_NSIsEmptyRect", w_NSIsEmptyRect, fnd);
    fn("_NSEqualRects", w_CGRectEqualToRect, fnd);
    fn("_NSEqualPoints", w_CGPointEqualToPoint, fnd);
    fn("_NSEqualSizes", w_CGSizeEqualToSize, fnd);
    fn("_NSIntersectionRect", w_CGRectIntersection, fnd);
    fn("_NSUnionRect", w_CGRectUnion, fnd);
    fn("_NSInsetRect", w_CGRectInset, fnd);
    fn("_NSOffsetRect", w_CGRectOffset, fnd);
    fn("_NSIntegralRect", w_CGRectIntegral, fnd);
    fn("_NSStringFromRect", w_NSStringFromRect, fnd);
    fn("_NSStringFromPoint", w_NSStringFromPoint, fnd);
    fn("_NSStringFromSize", w_NSStringFromSize, fnd);
    add_cg_api_symbols(t);
}

void install_cg(objc::Runtime& rt) {
    install_cg_api(rt);
    // Constant structures (CGRectZero...) live in the runtime arena; the symbols resolve to their addresses.
    auto table = std::make_shared<std::unordered_map<std::string, uint64_t>>();
    auto make = [&](const char* sym, std::initializer_list<double> v) {
        const uint64_t a = rt.arena_alloc(8 * v.size(), 16);
        double* d = reinterpret_cast<double*>(a);
        size_t i = 0;
        for (double x : v) d[i++] = x;
        (*table)[sym] = a;
    };
    const Rect null = geometry::null_rect(), inf = geometry::infinite_rect();
    make("_CGPointZero", {0, 0});
    make("_CGSizeZero", {0, 0});
    make("_CGRectZero", {0, 0, 0, 0});
    make("_CGRectNull", {null.origin.x, null.origin.y, 0, 0});
    make("_CGRectInfinite", {inf.origin.x, inf.origin.y, inf.size.width, inf.size.height});
    make("_NSZeroPoint", {0, 0});
    make("_NSZeroSize", {0, 0});
    make("_NSZeroRect", {0, 0, 0, 0});
    rt.add_dynamic_provider([table](std::string_view name) -> std::optional<uint64_t> {
        auto it = table->find(std::string(name));
        return it == table->end() ? std::nullopt : std::optional<uint64_t>(it->second);
    });
}

}  // namespace cider::graphics
