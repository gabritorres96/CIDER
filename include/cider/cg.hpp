#pragma once
// CoreGraphics drawing engine (CIDER 0.6.1): the graphics state machine, paths and the device interface behind the guest-visible CG* functions.
//
// Layering (see docs/COREGRAPHICS-RENDERING.md):
//   guest CG* calls (src/graphics/cg_api.cpp)  ->  Context (state stack, CTM, clip stack, current path; this header, pure C++)  ->  Device (drawing primitives)
//   Device implementations: Direct2D (HWND render target for windows, WIC bitmap render target for bitmap contexts) and a recording device used by tests.
// Nothing here knows about Objective-C, HWNDs or the guest.
//
// Coordinate model: user space is CoreGraphics user space (origin bottom-left unless the creator flips it). The creator of a Context supplies the BASE transform
// from user space to device pixels (flip, DPI scale); `ctm` is the user's own transform applied on top of it.
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "cider/geometry.hpp"

namespace cider::cg {

using geometry::Point;
using geometry::Rect;
using geometry::Size;

// ---- transform (a b c d tx ty), CGAffineTransform layout: x' = a*x + c*y + tx, y' = b*x + d*y + ty --------------------------------------------------
struct Affine {
    double a = 1, b = 0, c = 0, d = 1, tx = 0, ty = 0;
    static Affine translation(double x, double y) { return {1, 0, 0, 1, x, y}; }
    static Affine scaling(double sx, double sy) { return {sx, 0, 0, sy, 0, 0}; }
    static Affine rotation(double radians);
    Point apply(Point p) const { return {a * p.x + c * p.y + tx, b * p.x + d * p.y + ty}; }
    Size apply(Size s) const { return {a * s.width + c * s.height, b * s.width + d * s.height}; }   // no translation
    bool is_identity() const { return a == 1 && b == 0 && c == 0 && d == 1 && tx == 0 && ty == 0; }
    bool is_axis_aligned() const { return b == 0 && c == 0; }                                        // scale + translate only
    double determinant() const { return a * d - b * c; }
};
// CoreGraphics order: CGAffineTransformConcat(t1, t2) = t1 followed by t2 (apply t1 first)
Affine concat(const Affine& t1, const Affine& t2);
bool invert(const Affine& t, Affine& out);                       // false when singular
Rect transformed_bounds(const Rect& r, const Affine& t);         // bounding box of the transformed rectangle

struct Color {
    double r = 0, g = 0, b = 0, a = 1;
    bool operator==(const Color& o) const { return r == o.r && g == o.g && b == o.b && a == o.a; }
};

enum class FillRule { NonZero, EvenOdd };
enum class LineCap { Butt = 0, Round = 1, Square = 2 };
enum class LineJoin { Miter = 0, Round = 1, Bevel = 2 };
enum class PathMode { Fill = 0, EOFill = 1, Stroke = 2, FillStroke = 3, EOFillStroke = 4 };    // CGPathDrawingMode values

struct PathElement {
    enum Type { Move, Line, Cubic, Close } type = Move;
    Point p[3];                                                  // Move/Line: p[0]; Cubic: control1, control2, end
};
class Path {
public:
    void move_to(Point p);
    void line_to(Point p);
    void curve_to(Point c1, Point c2, Point e);
    void quad_to(Point c, Point e);                              // converted to a cubic
    void arc(Point center, double radius, double start, double end, bool clockwise);   // CGContextAddArc semantics (angles in radians, y up)
    void arc_to(Point p1, Point p2, double radius);
    void add_rect(const Rect& r);
    void add_ellipse(const Rect& r);
    void close();
    void add_path(const Path& other, const Affine& t);
    bool empty() const { return elements_.empty(); }
    bool has_current_point() const { return has_current_; }
    Point current_point() const { return current_; }
    Rect bounding_box() const;                                   // of the control points; null_rect() for an empty path
    bool is_rect(Rect& out) const;                               // a single axis-aligned rectangle (used to turn clips into rectangle clips)
    const std::vector<PathElement>& elements() const { return elements_; }
    size_t size() const { return elements_.size(); }
    bool contains(Point p, FillRule rule) const;                 // point in path after flattening curves (tests, hit testing)

private:
    std::vector<PathElement> elements_;
    Point current_, start_;
    bool has_current_ = false;
};

struct StrokeStyle {
    double width = 1;
    LineCap cap = LineCap::Butt;
    LineJoin join = LineJoin::Miter;
    double miter_limit = 10;
    std::vector<double> dash;
    double dash_phase = 0;
};

// ---- images --------------------------------------------------------------------------------------------------------------------------------------------
// Immutable BGRA (premultiplied) pixel image shared by CGImage and NSImage representations.
struct Image {
    uint32_t width = 0, height = 0;
    std::vector<uint8_t> bgra;                                   // width * height * 4, premultiplied alpha, row 0 at the top
    bool valid() const { return width && height && bgra.size() == static_cast<size_t>(width) * height * 4; }
};

struct FontSpec {
    std::string family = "Segoe UI";
    double size = 12;
    bool bold = false, italic = false;
};

// ---- device -----------------------------------------------------------------------------------------------------------------------------------------------
// Drawing primitives in DEVICE space (pixels, y down); every call carries the full transform (base * ctm). Implementations must keep clip push/pop strictly nested.
class Device {
public:
    virtual ~Device() = default;
    virtual const char* name() const = 0;
    virtual void clear(const Color& c) = 0;
    virtual void fill_path(const Path& p, const Affine& to_device, const Color& c, FillRule rule, bool antialias) = 0;
    virtual void stroke_path(const Path& p, const Affine& to_device, const Color& c, const StrokeStyle& s, bool antialias) = 0;
    virtual void push_clip(const Path& p, const Affine& to_device, FillRule rule) = 0;
    virtual void pop_clip() = 0;
    // `dst` in user space (the image is stretched to it, row 0 at the top edge of the TOP of dst as seen by the user: the context flips as needed)
    virtual void draw_image(const Image& img, const Rect& dst, const Affine& to_device, double alpha, bool interpolate) = 0;
    virtual void draw_text(const std::u16string& text, const FontSpec& font, Point origin_user, const Affine& to_device, const Color& c) = 0;
    virtual Size measure_text(const std::u16string& text, const FontSpec& font) = 0;
    virtual bool ok() const = 0;                                 // false after an unrecoverable device error (e.g. lost target)
};

// ---- context ---------------------------------------------------------------------------------------------------------------------------------------------
struct GState {
    Affine ctm;                                                  // user transform (on top of the base transform)
    Color fill{0, 0, 0, 1}, stroke{0, 0, 0, 1};
    StrokeStyle line;
    double alpha = 1;
    bool antialias = true;
    bool interpolate = true;
    int clips_pushed = 0;                                        // device clips pushed by this state (popped on restore)
    Rect clip_bounds;                                            // conservative bounding box of the clip in user space (null_rect() = none)
};

class Context {
public:
    Context(Device* device, const Affine& base, Size size_in_points);
    ~Context();                                                  // pops every clip still pushed

    // state
    void save();
    bool restore();                                              // false on an unbalanced restore (the guest is told; nothing is popped)
    size_t depth() const { return stack_.size(); }               // number of saved states
    const GState& state() const { return gs_; }
    GState& mutable_state() { return gs_; }
    Affine device_matrix() const { return concat(gs_.ctm, base_); }
    Affine base() const { return base_; }
    void set_base(const Affine& b) { base_ = b; }
    Size size() const { return size_; }

    // transform
    void translate(double x, double y) { gs_.ctm = concat(Affine::translation(x, y), gs_.ctm); }
    void scale(double sx, double sy) { gs_.ctm = concat(Affine::scaling(sx, sy), gs_.ctm); }
    void rotate(double radians) { gs_.ctm = concat(Affine::rotation(radians), gs_.ctm); }
    void concat_ctm(const Affine& t) { gs_.ctm = concat(t, gs_.ctm); }

    // current path (user space, stored with the CTM that was current when each point was added: points are transformed when added, like CoreGraphics)
    Path& path() { return path_; }
    void begin_path() { path_ = Path(); }
    void add_path(const Path& p) { path_.add_path(p, gs_.ctm); }
    Point to_device_space_point(Point user) const { return gs_.ctm.apply(user); }

    // painting
    void fill_rect(const Rect& r);
    void stroke_rect(const Rect& r);
    void clear_rect(const Rect& r);
    void fill_current(FillRule rule);
    void stroke_current();
    void draw_current(PathMode mode);
    void clip_current(FillRule rule);
    void clip_to_rect(const Rect& r);
    void draw_image(const Image& img, const Rect& dst);
    void draw_text(const std::u16string& text, const FontSpec& font, Point origin);
    Device* device() const { return device_; }

private:
    // paths in `path_` are kept in USER space already transformed by the CTM at insertion time (so a later CTM change does not move them);
    // painting therefore uses only the base transform.
    Device* device_;
    Affine base_;
    Size size_;
    GState gs_;
    std::vector<GState> stack_;
    Path path_;
};

// the base transform of a context drawing into a target of `height_pixels` pixels with the origin at the bottom-left (CoreGraphics bitmap default) and a pixel scale
Affine bottom_left_base(double height_in_points, double scale);
// base transform of an AppKit view context: unflipped views (y up) or flipped ones (y down); `scale` converts points to pixels
Affine view_base(double view_height_points, bool flipped, double scale);

}  // namespace cider::cg
