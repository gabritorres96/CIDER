// CoreGraphics graphics-state machine, paths and transforms (pure C++, no Windows, no guest access). See include/cider/cg.hpp.
#include <algorithm>
#include <cmath>

#include "cider/cg.hpp"

namespace cider::cg {
namespace {
constexpr double kPi = 3.14159265358979323846;
constexpr double kKappa = 0.5522847498307936;           // 4/3 * (sqrt(2) - 1): cubic approximation of a quarter circle

Point lerp(Point a, Point b, double t) { return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t}; }

void flatten(const Path& p, std::vector<std::vector<Point>>& polys) {
    Point cur{0, 0};
    for (const auto& e : p.elements()) {
        switch (e.type) {
            case PathElement::Move: polys.emplace_back(); polys.back().push_back(e.p[0]); cur = e.p[0]; break;
            case PathElement::Line: if (polys.empty()) polys.emplace_back(); polys.back().push_back(e.p[0]); cur = e.p[0]; break;
            case PathElement::Cubic: {
                if (polys.empty()) polys.emplace_back();
                const int steps = 24;
                for (int i = 1; i <= steps; ++i) {
                    const double t = static_cast<double>(i) / steps, u = 1 - t;
                    const Point q{u * u * u * cur.x + 3 * u * u * t * e.p[0].x + 3 * u * t * t * e.p[1].x + t * t * t * e.p[2].x,
                                  u * u * u * cur.y + 3 * u * u * t * e.p[0].y + 3 * u * t * t * e.p[1].y + t * t * t * e.p[2].y};
                    polys.back().push_back(q);
                }
                cur = e.p[2];
                break;
            }
            case PathElement::Close: if (!polys.empty() && !polys.back().empty()) { cur = polys.back().front(); polys.back().push_back(cur); } break;
        }
    }
}
}  // namespace

// ---- Affine ------------------------------------------------------------------------------------------------------------------------------------------------
Affine Affine::rotation(double r) {
    const double c = std::cos(r), s = std::sin(r);
    return {c, s, -s, c, 0, 0};
}
Affine concat(const Affine& t1, const Affine& t2) {                   // apply t1, then t2
    return {t1.a * t2.a + t1.b * t2.c, t1.a * t2.b + t1.b * t2.d, t1.c * t2.a + t1.d * t2.c, t1.c * t2.b + t1.d * t2.d,
            t1.tx * t2.a + t1.ty * t2.c + t2.tx, t1.tx * t2.b + t1.ty * t2.d + t2.ty};
}
bool invert(const Affine& t, Affine& out) {
    const double det = t.determinant();
    if (det == 0 || !std::isfinite(det)) return false;
    out.a = t.d / det;
    out.b = -t.b / det;
    out.c = -t.c / det;
    out.d = t.a / det;
    out.tx = (t.c * t.ty - t.d * t.tx) / det;
    out.ty = (t.b * t.tx - t.a * t.ty) / det;
    return true;
}
Rect transformed_bounds(const Rect& r, const Affine& t) {
    const Rect s = geometry::standardize(r);
    const Point c[4] = {t.apply(Point{s.min_x(), s.min_y()}), t.apply(Point{s.max_x(), s.min_y()}), t.apply(Point{s.max_x(), s.max_y()}), t.apply(Point{s.min_x(), s.max_y()})};
    double x0 = c[0].x, x1 = c[0].x, y0 = c[0].y, y1 = c[0].y;
    for (const Point& p : c) { x0 = std::min(x0, p.x); x1 = std::max(x1, p.x); y0 = std::min(y0, p.y); y1 = std::max(y1, p.y); }
    return geometry::make_rect(x0, y0, x1 - x0, y1 - y0);
}

namespace {
bool finite(Point p) { return std::isfinite(p.x) && std::isfinite(p.y); }
}  // namespace

// ---- Path ----------------------------------------------------------------------------------------------------------------------------------------------------
void Path::move_to(Point p) {
    if (!finite(p)) return;                                              // CoreGraphics ignores NaN / infinite coordinates (with a log); so does the engine
    PathElement e;
    e.type = PathElement::Move;
    e.p[0] = p;
    elements_.push_back(e);
    current_ = start_ = p;
    has_current_ = true;
}
void Path::line_to(Point p) {
    if (!finite(p)) return;
    if (!has_current_) { move_to(p); return; }
    PathElement e;
    e.type = PathElement::Line;
    e.p[0] = p;
    elements_.push_back(e);
    current_ = p;
}
void Path::curve_to(Point c1, Point c2, Point end) {
    if (!finite(c1) || !finite(c2) || !finite(end)) return;
    if (!has_current_) move_to(c1);
    PathElement e;
    e.type = PathElement::Cubic;
    e.p[0] = c1; e.p[1] = c2; e.p[2] = end;
    elements_.push_back(e);
    current_ = end;
}
void Path::quad_to(Point c, Point e) {
    if (!has_current_) move_to(c);
    const Point p0 = current_;
    curve_to({p0.x + 2.0 / 3.0 * (c.x - p0.x), p0.y + 2.0 / 3.0 * (c.y - p0.y)}, {e.x + 2.0 / 3.0 * (c.x - e.x), e.y + 2.0 / 3.0 * (c.y - e.y)}, e);
}
void Path::close() {
    if (!has_current_) return;
    PathElement e;
    e.type = PathElement::Close;
    elements_.push_back(e);
    current_ = start_;
}
void Path::add_rect(const Rect& r) {
    const Rect s = geometry::standardize(r);
    move_to({s.min_x(), s.min_y()});
    line_to({s.max_x(), s.min_y()});
    line_to({s.max_x(), s.max_y()});
    line_to({s.min_x(), s.max_y()});
    close();
}
void Path::add_ellipse(const Rect& r) {
    const Rect s = geometry::standardize(r);
    const double cx = s.mid_x(), cy = s.mid_y(), rx = s.width() / 2, ry = s.height() / 2;
    const double kx = rx * kKappa, ky = ry * kKappa;
    move_to({cx + rx, cy});
    curve_to({cx + rx, cy + ky}, {cx + kx, cy + ry}, {cx, cy + ry});
    curve_to({cx - kx, cy + ry}, {cx - rx, cy + ky}, {cx - rx, cy});
    curve_to({cx - rx, cy - ky}, {cx - kx, cy - ry}, {cx, cy - ry});
    curve_to({cx + kx, cy - ry}, {cx + rx, cy - ky}, {cx + rx, cy});
    close();
}
void Path::arc(Point c, double radius, double start, double end, bool clockwise) {
    if (!finite(c) || !std::isfinite(radius) || !std::isfinite(start) || !std::isfinite(end) || std::fabs(radius) > 1e12) return;
    double sweep = end - start;
    if (clockwise) { if (sweep > 0) { sweep = std::fmod(sweep, 2 * kPi); if (sweep > 0) sweep -= 2 * kPi; } if (sweep < -2 * kPi) sweep = -2 * kPi; }
    else { if (sweep < 0) { sweep = std::fmod(sweep, 2 * kPi); if (sweep < 0) sweep += 2 * kPi; } if (sweep > 2 * kPi) sweep = 2 * kPi; }
    const Point p0{c.x + radius * std::cos(start), c.y + radius * std::sin(start)};
    if (has_current_) line_to(p0); else move_to(p0);
    if (sweep == 0) return;
    const int segments = std::max(1, static_cast<int>(std::ceil(std::fabs(sweep) / (kPi / 2) - 1e-9)));
    const double step = sweep / segments;
    const double k = 4.0 / 3.0 * std::tan(step / 4);
    double a = start;
    for (int i = 0; i < segments; ++i) {
        const double a1 = a + step;
        const Point s{c.x + radius * std::cos(a), c.y + radius * std::sin(a)}, e{c.x + radius * std::cos(a1), c.y + radius * std::sin(a1)};
        curve_to({s.x - k * radius * std::sin(a), s.y + k * radius * std::cos(a)}, {e.x + k * radius * std::sin(a1), e.y - k * radius * std::cos(a1)}, e);
        a = a1;
    }
}
void Path::arc_to(Point p1, Point p2, double radius) {
    if (!finite(p1) || !finite(p2) || !std::isfinite(radius)) return;
    if (!has_current_) { move_to(p1); return; }
    const Point p0 = current_;
    const double ux = p0.x - p1.x, uy = p0.y - p1.y, vx = p2.x - p1.x, vy = p2.y - p1.y;
    const double lu = std::hypot(ux, uy), lv = std::hypot(vx, vy);
    if (lu == 0 || lv == 0 || radius <= 0) { line_to(p1); return; }
    const double cross = ux * vy - uy * vx;
    if (std::fabs(cross) < 1e-12) { line_to(p1); return; }              // collinear
    const double cosang = (ux * vx + uy * vy) / (lu * lv);
    const double half = std::acos(std::clamp(cosang, -1.0, 1.0)) / 2;
    const double dist = radius / std::tan(half);
    const Point t0{p1.x + ux / lu * dist, p1.y + uy / lu * dist}, t1{p1.x + vx / lv * dist, p1.y + vy / lv * dist};
    const double bx = ux / lu + vx / lv, by = uy / lu + vy / lv, bl = std::hypot(bx, by);
    const double cd = radius / std::sin(half);
    const Point center{p1.x + bx / bl * cd, p1.y + by / bl * cd};
    const double a0 = std::atan2(t0.y - center.y, t0.x - center.x), a1 = std::atan2(t1.y - center.y, t1.x - center.x);
    line_to(t0);
    arc(center, radius, a0, a1, cross > 0);
}
void Path::add_path(const Path& other, const Affine& t, bool connect_first) {
    bool first = true;
    for (const auto& e : other.elements_) {
        const bool lead = first;
        first = false;
        switch (e.type) {
            case PathElement::Move:
                if (lead && connect_first && has_current_) line_to(t.apply(e.p[0])); else move_to(t.apply(e.p[0]));
                break;
            case PathElement::Line: line_to(t.apply(e.p[0])); break;
            case PathElement::Cubic: curve_to(t.apply(e.p[0]), t.apply(e.p[1]), t.apply(e.p[2])); break;
            case PathElement::Close: close(); break;
        }
    }
}
Rect Path::bounding_box() const {
    bool any = false;
    double x0 = 0, x1 = 0, y0 = 0, y1 = 0;
    auto add = [&](Point p) {
        if (!any) { x0 = x1 = p.x; y0 = y1 = p.y; any = true; return; }
        x0 = std::min(x0, p.x); x1 = std::max(x1, p.x); y0 = std::min(y0, p.y); y1 = std::max(y1, p.y);
    };
    for (const auto& e : elements_) {
        switch (e.type) {
            case PathElement::Move: case PathElement::Line: add(e.p[0]); break;
            case PathElement::Cubic: add(e.p[0]); add(e.p[1]); add(e.p[2]); break;
            case PathElement::Close: break;
        }
    }
    return any ? geometry::make_rect(x0, y0, x1 - x0, y1 - y0) : geometry::null_rect();
}
bool Path::is_rect(Rect& out) const {
    // move, 3 lines (closed or not), optional close; all segments axis aligned
    std::vector<Point> pts;
    for (const auto& e : elements_) {
        if (e.type == PathElement::Move && pts.empty()) pts.push_back(e.p[0]);
        else if (e.type == PathElement::Line) pts.push_back(e.p[0]);
        else if (e.type == PathElement::Close) {}
        else return false;
    }
    if (pts.size() == 5 && pts[4].x == pts[0].x && pts[4].y == pts[0].y) pts.pop_back();
    if (pts.size() != 4) return false;
    const bool a = pts[0].y == pts[1].y && pts[1].x == pts[2].x && pts[2].y == pts[3].y && pts[3].x == pts[0].x;
    const bool b = pts[0].x == pts[1].x && pts[1].y == pts[2].y && pts[2].x == pts[3].x && pts[3].y == pts[0].y;
    if (!a && !b) return false;
    out = bounding_box();
    return true;
}
bool Path::contains(Point p, FillRule rule) const {
    std::vector<std::vector<Point>> polys;
    flatten(*this, polys);
    int winding = 0, crossings = 0;
    for (auto& poly : polys) {
        if (poly.size() < 2) continue;
        const size_t n = poly.size();
        for (size_t i = 0; i < n; ++i) {                                  // implicit closing edge
            const Point a = poly[i], b = poly[(i + 1) % n];
            if ((a.y <= p.y) != (b.y <= p.y)) {
                const double x = a.x + (p.y - a.y) / (b.y - a.y) * (b.x - a.x);
                if (x > p.x) { ++crossings; winding += b.y > a.y ? 1 : -1; }
            }
        }
    }
    return rule == FillRule::EvenOdd ? (crossings & 1) != 0 : winding != 0;
}

// ---- Context -------------------------------------------------------------------------------------------------------------------------------------------
Context::Context(Device* device, const Affine& base, Size size) : device_(device), base_(base), size_(size) { gs_.clip_bounds = geometry::null_rect(); }
Context::~Context() {
    for (int i = 0; i < gs_.clips_pushed; ++i) device_->pop_clip();
    for (auto it = stack_.rbegin(); it != stack_.rend(); ++it) for (int i = 0; i < it->clips_pushed; ++i) device_->pop_clip();
}
void Context::save() {
    stack_.push_back(gs_);
    gs_.clips_pushed = 0;                                               // clips pushed from here on belong to the new state
}
bool Context::restore() {
    if (stack_.empty()) return false;
    for (int i = 0; i < gs_.clips_pushed; ++i) device_->pop_clip();
    gs_ = stack_.back();
    stack_.pop_back();
    return true;
}

namespace {
double ctm_scale(const Affine& t) { return std::sqrt(std::fabs(t.determinant())); }
Color with_alpha(Color c, double a) { c.a *= a; return c; }
}  // namespace

void Context::fill_rect(const Rect& r) {
    Path p;
    p.add_rect(r);
    Path q;
    q.add_path(p, gs_.ctm);
    device_->fill_path(q, base_, with_alpha(gs_.fill, gs_.alpha), FillRule::NonZero, gs_.antialias);
}
void Context::stroke_rect(const Rect& r) {
    Path p;
    p.add_rect(r);
    Path q;
    q.add_path(p, gs_.ctm);
    StrokeStyle s = gs_.line;
    const double k = ctm_scale(gs_.ctm);
    s.width *= k;
    for (double& d : s.dash) d *= k;
    device_->stroke_path(q, base_, with_alpha(gs_.stroke, gs_.alpha), s, gs_.antialias);
}
void Context::clear_rect(const Rect& r) {
    const Rect devr = transformed_bounds(r, device_matrix());          // axis-aligned bounding box when the transform rotates (documented)
    Path p;
    p.add_rect(devr);
    device_->push_clip(p, Affine{}, FillRule::NonZero);
    device_->clear(Color{0, 0, 0, 0});
    device_->pop_clip();
}
void Context::fill_current(FillRule rule) {
    if (!path_.empty()) device_->fill_path(path_, base_, with_alpha(gs_.fill, gs_.alpha), rule, gs_.antialias);
    begin_path();
}
void Context::stroke_current() {
    if (!path_.empty()) {
        StrokeStyle s = gs_.line;
        const double k = ctm_scale(gs_.ctm);
        s.width *= k;
        for (double& d : s.dash) d *= k;
        device_->stroke_path(path_, base_, with_alpha(gs_.stroke, gs_.alpha), s, gs_.antialias);
    }
    begin_path();
}
void Context::draw_current(PathMode mode) {
    if (path_.empty()) { begin_path(); return; }
    const bool fill = mode != PathMode::Stroke, stroke = mode == PathMode::Stroke || mode == PathMode::FillStroke || mode == PathMode::EOFillStroke;
    const FillRule rule = (mode == PathMode::EOFill || mode == PathMode::EOFillStroke) ? FillRule::EvenOdd : FillRule::NonZero;
    if (fill) device_->fill_path(path_, base_, with_alpha(gs_.fill, gs_.alpha), rule, gs_.antialias);
    if (stroke) {
        StrokeStyle s = gs_.line;
        const double k = ctm_scale(gs_.ctm);
        s.width *= k;
        for (double& d : s.dash) d *= k;
        device_->stroke_path(path_, base_, with_alpha(gs_.stroke, gs_.alpha), s, gs_.antialias);
    }
    begin_path();
}
void Context::clip_current(FillRule rule) {
    if (path_.empty()) {                                                // CoreGraphics: clipping to an empty path clips everything
        Path none;
        none.add_rect(geometry::make_rect(0, 0, 0, 0));
        device_->push_clip(none, base_, rule);
        ++gs_.clips_pushed;
        gs_.clip_bounds = geometry::make_rect(0, 0, 0, 0);
    } else {
        device_->push_clip(path_, base_, rule);
        ++gs_.clips_pushed;
        const Rect b = path_.bounding_box();
        gs_.clip_bounds = geometry::is_null(gs_.clip_bounds) ? b : geometry::intersection(gs_.clip_bounds, b);
        if (geometry::is_null(gs_.clip_bounds)) gs_.clip_bounds = geometry::make_rect(0, 0, 0, 0);
    }
    begin_path();
}
void Context::clip_to_rect(const Rect& r) {
    begin_path();
    Path p;
    p.add_rect(r);
    path_.add_path(p, gs_.ctm);
    clip_current(FillRule::NonZero);
}
void Context::draw_image(const Image& img, const Rect& dst) {
    if (!img.valid()) return;
    device_->draw_image(img, dst, device_matrix(), gs_.alpha, gs_.interpolate);
}
void Context::draw_text(const std::u16string& text, const FontSpec& font, Point origin) {
    device_->draw_text(text, font, origin, device_matrix(), with_alpha(gs_.fill, gs_.alpha));
}

Affine bottom_left_base(double height_in_points, double scale) { return {scale, 0, 0, -scale, 0, height_in_points * scale}; }
Affine view_base(double view_height_points, bool flipped, double scale) { return flipped ? Affine{scale, 0, 0, scale, 0, 0} : bottom_left_base(view_height_points, scale); }

}  // namespace cider::cg
