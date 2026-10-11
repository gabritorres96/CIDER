#include "orvyn/geometry.hpp"

#include <algorithm>

namespace orvyn::geometry {

bool is_null(const Rect& r) { return std::isinf(r.origin.x) || std::isinf(r.origin.y); }
bool is_infinite(const Rect& r) {
    const Rect i = infinite_rect();
    return r.origin.x == i.origin.x && r.origin.y == i.origin.y && r.size.width == i.size.width && r.size.height == i.size.height;
}
bool is_empty(const Rect& r) { return is_null(r) || r.size.width == 0 || r.size.height == 0; }
bool equal(const Rect& a, const Rect& b) { return a.origin.x == b.origin.x && a.origin.y == b.origin.y && a.size.width == b.size.width && a.size.height == b.size.height; }
bool equal(const Point& a, const Point& b) { return a.x == b.x && a.y == b.y; }
bool equal(const Size& a, const Size& b) { return a.width == b.width && a.height == b.height; }

Rect standardize(const Rect& r) {
    if (is_null(r)) return r;
    return make_rect(r.min_x(), r.min_y(), r.width(), r.height());
}
Rect inset(const Rect& r, double dx, double dy) {
    if (is_null(r)) return r;
    Rect s = standardize(r);
    s.origin.x += dx;
    s.origin.y += dy;
    s.size.width -= 2 * dx;
    s.size.height -= 2 * dy;
    if (s.size.width < 0 || s.size.height < 0) return null_rect();
    return s;
}
Rect offset(const Rect& r, double dx, double dy) {
    if (is_null(r)) return r;
    Rect o = r;
    o.origin.x += dx;
    o.origin.y += dy;
    return o;
}
Rect integral(const Rect& r) {
    if (is_null(r)) return r;
    const Rect s = standardize(r);
    const double x0 = std::floor(s.min_x()), y0 = std::floor(s.min_y());
    const double x1 = std::ceil(s.max_x()), y1 = std::ceil(s.max_y());
    return make_rect(x0, y0, x1 - x0, y1 - y0);
}
Rect union_rect(const Rect& a, const Rect& b) {
    if (is_null(a)) return b;
    if (is_null(b)) return a;
    const Rect sa = standardize(a), sb = standardize(b);
    const double x0 = std::min(sa.min_x(), sb.min_x()), y0 = std::min(sa.min_y(), sb.min_y());
    const double x1 = std::max(sa.max_x(), sb.max_x()), y1 = std::max(sa.max_y(), sb.max_y());
    return make_rect(x0, y0, x1 - x0, y1 - y0);
}
Rect intersection(const Rect& a, const Rect& b) {
    if (is_null(a) || is_null(b)) return null_rect();
    const Rect sa = standardize(a), sb = standardize(b);
    const double x0 = std::max(sa.min_x(), sb.min_x()), y0 = std::max(sa.min_y(), sb.min_y());
    const double x1 = std::min(sa.max_x(), sb.max_x()), y1 = std::min(sa.max_y(), sb.max_y());
    if (x1 <= x0 || y1 <= y0) return null_rect();
    return make_rect(x0, y0, x1 - x0, y1 - y0);
}
bool contains(const Rect& r, const Point& p) {
    const Rect s = standardize(r);
    return !is_null(r) && p.x >= s.min_x() && p.x < s.max_x() && p.y >= s.min_y() && p.y < s.max_y();
}
bool contains(const Rect& outer, const Rect& inner) { return equal(union_rect(outer, inner), standardize(outer)); }
bool intersects(const Rect& a, const Rect& b) { return !is_null(intersection(a, b)); }

double scale_for_dpi(unsigned dpi) { return std::clamp(static_cast<double>(dpi == 0 ? 96 : dpi) / 96.0, 0.5, 8.0); }
long to_pixels(double points, double scale) { return static_cast<long>(std::lround(points * scale)); }
double to_points(long pixels, double scale) { return static_cast<double>(pixels) / scale; }

PixelRect screen_to_win32(const Rect& r, double scale, long screen_height_px) {
    const Rect s = standardize(r);
    PixelRect p;
    p.left = to_pixels(s.min_x(), scale);
    p.right = to_pixels(s.max_x(), scale);
    p.top = screen_height_px - to_pixels(s.max_y(), scale);
    p.bottom = screen_height_px - to_pixels(s.min_y(), scale);
    return p;
}
Rect win32_to_screen(const PixelRect& p, double scale, long screen_height_px) {
    const double x = to_points(p.left, scale), w = to_points(p.right - p.left, scale), h = to_points(p.bottom - p.top, scale);
    const double y = to_points(screen_height_px - p.bottom, scale);
    return make_rect(x, y, w, h);
}
PixelRect child_to_client(const Rect& frame, double parent_height, bool parent_flipped, double scale) {
    const Rect s = standardize(frame);
    PixelRect p;
    p.left = to_pixels(s.min_x(), scale);
    p.right = to_pixels(s.max_x(), scale);
    if (parent_flipped) {
        p.top = to_pixels(s.min_y(), scale);
        p.bottom = to_pixels(s.max_y(), scale);
    } else {
        p.top = to_pixels(parent_height - s.max_y(), scale);
        p.bottom = to_pixels(parent_height - s.min_y(), scale);
    }
    return p;
}
Rect client_to_child(const PixelRect& p, double parent_height, bool parent_flipped, double scale) {
    const double x = to_points(p.left, scale), w = to_points(p.right - p.left, scale), h = to_points(p.bottom - p.top, scale);
    const double y = parent_flipped ? to_points(p.top, scale) : parent_height - to_points(p.bottom, scale);
    return make_rect(x, y, w, h);
}

Rect autoresize(const Rect& frame, const Size& old_size, const Size& new_size, unsigned mask) {
    constexpr unsigned kMinX = 1, kWidth = 2, kMaxX = 4, kMinY = 8, kHeight = 16, kMaxY = 32;
    Rect r = frame;
    auto axis = [](double& pos, double& len, double old_total, double new_total, bool min_margin, bool sizable, bool max_margin) {
        const double delta = new_total - old_total;
        const unsigned flex = (min_margin ? 1u : 0u) + (sizable ? 1u : 0u) + (max_margin ? 1u : 0u);
        if (flex == 0 || delta == 0) return;
        const double share = delta / flex;
        if (min_margin) pos += share;
        if (sizable) len += share;
    };
    axis(r.origin.x, r.size.width, old_size.width, new_size.width, (mask & kMinX) != 0, (mask & kWidth) != 0, (mask & kMaxX) != 0);
    axis(r.origin.y, r.size.height, old_size.height, new_size.height, (mask & kMinY) != 0, (mask & kHeight) != 0, (mask & kMaxY) != 0);
    return r;
}

}  // namespace orvyn::geometry
