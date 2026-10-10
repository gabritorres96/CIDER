#pragma once
// Geometry shared by the CoreGraphics/AppKit emulation and the Win32 backend (see docs/COREGRAPHICS.md). Pure functions: no Windows, no guest access.
//
// Conventions that differ between the two worlds (never assume they match):
//   * AppKit/CoreGraphics screen space: origin at the BOTTOM-left of the primary display, y grows UP, units are points.
//   * AppKit views: unflipped views have their origin at the bottom-left of their bounds; flipped views at the top-left.
//   * Win32 screen space: origin at the TOP-left of the virtual desktop, y grows DOWN, units are device pixels.
//   * A point is converted to pixels with the monitor scale (dpi / 96): at 100 % a point is one pixel. There is no Retina backing store.
#include <cmath>
#include <limits>

namespace cider::geometry {

struct Point { double x = 0, y = 0; };
struct Size { double width = 0, height = 0; };
struct Rect {
    Point origin;
    Size size;
    double min_x() const { return size.width < 0 ? origin.x + size.width : origin.x; }
    double max_x() const { return size.width < 0 ? origin.x : origin.x + size.width; }
    double min_y() const { return size.height < 0 ? origin.y + size.height : origin.y; }
    double max_y() const { return size.height < 0 ? origin.y : origin.y + size.height; }
    double mid_x() const { return (min_x() + max_x()) / 2; }
    double mid_y() const { return (min_y() + max_y()) / 2; }
    double width() const { return std::fabs(size.width); }
    double height() const { return std::fabs(size.height); }
};

inline Rect make_rect(double x, double y, double w, double h) { return Rect{{x, y}, {w, h}}; }
inline Rect null_rect() { const double inf = std::numeric_limits<double>::infinity(); return Rect{{inf, inf}, {0, 0}}; }
inline Rect infinite_rect() { const double m = std::numeric_limits<double>::max(); return Rect{{-m / 2, -m / 2}, {m, m}}; }

bool is_null(const Rect& r);
bool is_infinite(const Rect& r);
bool is_empty(const Rect& r);                                    // null, or zero width or height
bool equal(const Rect& a, const Rect& b);
bool equal(const Point& a, const Point& b);
bool equal(const Size& a, const Size& b);
Rect standardize(const Rect& r);                                 // non-negative size, origin moved accordingly
Rect inset(const Rect& r, double dx, double dy);
Rect offset(const Rect& r, double dx, double dy);
Rect integral(const Rect& r);                                    // smallest rectangle with integer coordinates that contains r
Rect union_rect(const Rect& a, const Rect& b);
Rect intersection(const Rect& a, const Rect& b);                 // null_rect() when disjoint (touching edges do not intersect)
bool contains(const Rect& r, const Point& p);                    // half-open: min <= p < max, like CGRectContainsPoint
bool contains(const Rect& outer, const Rect& inner);
bool intersects(const Rect& a, const Rect& b);

// ---- AppKit <-> Win32 ------------------------------------------------------------------------------------------------------------------
struct PixelRect { long left = 0, top = 0, right = 0, bottom = 0; };     // Win32 RECT layout
double scale_for_dpi(unsigned dpi);                                       // dpi / 96, clamped to [0.5, 8]; 0 means 96
long to_pixels(double points, double scale);                              // rounds half away from zero
double to_points(long pixels, double scale);
// A rectangle in AppKit screen space (bottom-left origin) to a Win32 screen rectangle. `screen_height_px` is the height of the primary
// display in pixels: the y axis flips around it.
PixelRect screen_to_win32(const Rect& r, double scale, long screen_height_px);
Rect win32_to_screen(const PixelRect& p, double scale, long screen_height_px);
// A child view frame inside its parent: parent coordinates (unflipped parent: y up from its bottom edge; flipped parent: y down) to
// the parent's client-area pixels (y down).
PixelRect child_to_client(const Rect& frame, double parent_height_points, bool parent_flipped, double scale);
Rect client_to_child(const PixelRect& p, double parent_height_points, bool parent_flipped, double scale);

// NSView autoresizing (NSViewMinXMargin=1, WidthSizable=2, MaxXMargin=4, MinYMargin=8, HeightSizable=16, MaxYMargin=32): the new frame of
// a subview after its superview changed size. The extra space is shared evenly between the flexible parts of each axis.
Rect autoresize(const Rect& frame, const Size& old_size, const Size& new_size, unsigned mask);

}  // namespace cider::geometry
