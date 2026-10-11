// Geometry module (ORVYN 0.5.6): rectangle algebra with CoreGraphics semantics, AppKit <-> Win32 coordinate conversion, DPI and autoresizing.
#include "orvyn/geometry.hpp"
#include "test_framework.hpp"

using namespace orvyn::geometry;

ORVYN_TEST(rectangle_algebra_matches_coregraphics) {
    const Rect a = make_rect(0, 0, 10, 10), b = make_rect(5, 5, 10, 10);
    CHECK(equal(union_rect(a, b), make_rect(0, 0, 15, 15)));
    CHECK(equal(intersection(a, b), make_rect(5, 5, 5, 5)));
    CHECK(is_null(intersection(a, make_rect(20, 20, 1, 1))));
    CHECK(is_null(intersection(a, make_rect(10, 0, 5, 5))));        // touching edges do not intersect
    CHECK(intersects(a, b) && !intersects(a, make_rect(10, 10, 1, 1)));
    CHECK(contains(a, Point{0, 0}) && !contains(a, Point{10, 10}) && contains(a, Point{9.99, 9.99}));
    CHECK(contains(a, make_rect(2, 2, 3, 3)) && !contains(a, b));
    CHECK(equal(standardize(make_rect(10, 10, -4, -6)), make_rect(6, 4, 4, 6)));
    CHECK(equal(inset(a, 1, 2), make_rect(1, 2, 8, 6)) && is_null(inset(a, 6, 0)));
    CHECK(equal(offset(a, 3, -2), make_rect(3, -2, 10, 10)));
    CHECK(equal(integral(make_rect(0.5, 0.5, 1.2, 1.2)), make_rect(0, 0, 2, 2)));
    CHECK(is_empty(make_rect(0, 0, 0, 5)) && is_empty(null_rect()) && !is_empty(a));
    CHECK(is_null(null_rect()) && is_infinite(infinite_rect()));
    CHECK(equal(union_rect(null_rect(), a), a) && equal(union_rect(a, null_rect()), a));
    CHECK_EQ(a.mid_x(), 5.0);
    CHECK_EQ(make_rect(10, 0, -4, 1).min_x(), 6.0);                 // negative widths are standardized by the accessors
}

ORVYN_TEST(appkit_screen_space_flips_around_the_primary_display_height) {
    // 1080 px high display, 100 % scale: a 300x200 window whose bottom-left is at (100, 50) has its top at 1080 - 250 = 830
    const PixelRect p = screen_to_win32(make_rect(100, 50, 300, 200), 1.0, 1080);
    CHECK(p.left == 100 && p.right == 400 && p.top == 830 && p.bottom == 1030);
    CHECK(equal(win32_to_screen(p, 1.0, 1080), make_rect(100, 50, 300, 200)));
    // 150 % scale (144 dpi): points become 1.5 pixels
    const PixelRect q = screen_to_win32(make_rect(100, 50, 300, 200), 1.5, 1620);
    CHECK(q.left == 150 && q.right == 600 && q.bottom == 1620 - 75 && q.top == 1620 - 375);
    CHECK(equal(win32_to_screen(q, 1.5, 1620), make_rect(100, 50, 300, 200)));
    CHECK_EQ(scale_for_dpi(96), 1.0);
    CHECK_EQ(scale_for_dpi(144), 1.5);
    CHECK_EQ(scale_for_dpi(0), 1.0);                                  // unknown dpi means 96
    CHECK_EQ(scale_for_dpi(100000), 8.0);                             // clamped
    CHECK_EQ(to_pixels(2.5, 1.0), 3L);                                // rounds half away from zero
    CHECK_EQ(to_pixels(-2.5, 1.0), -3L);
}

ORVYN_TEST(child_frames_flip_inside_unflipped_parents_and_not_inside_flipped_ones) {
    // parent 400x300 points; child at (10, 20) size 100x30 measured from the parent bottom-left corner
    const PixelRect u = child_to_client(make_rect(10, 20, 100, 30), 300, false, 1.0);
    CHECK(u.left == 10 && u.right == 110 && u.top == 250 && u.bottom == 280);
    CHECK(equal(client_to_child(u, 300, false, 1.0), make_rect(10, 20, 100, 30)));
    const PixelRect f = child_to_client(make_rect(10, 20, 100, 30), 300, true, 1.0);
    CHECK(f.top == 20 && f.bottom == 50);
    CHECK(equal(client_to_child(f, 300, true, 1.0), make_rect(10, 20, 100, 30)));
    const PixelRect hi = child_to_client(make_rect(10, 20, 100, 30), 300, false, 2.0);
    CHECK(hi.left == 20 && hi.right == 220 && hi.top == 500 && hi.bottom == 560);
}

ORVYN_TEST(autoresizing_distributes_the_extra_space_like_nsview) {
    const Rect f = make_rect(10, 10, 100, 50);
    const Size from{200, 100}, to{300, 160};
    CHECK(equal(autoresize(f, from, to, 0), f));                                          // fixed
    CHECK(equal(autoresize(f, from, to, 2 | 16), make_rect(10, 10, 200, 110)));          // width and height sizable
    CHECK(equal(autoresize(f, from, to, 1), make_rect(110, 10, 100, 50)));               // flexible left margin: sticks to the right edge
    CHECK(equal(autoresize(f, from, to, 4), f));                                          // flexible right margin: stays at the left
    CHECK(equal(autoresize(f, from, to, 8), make_rect(10, 70, 100, 50)));                // flexible bottom margin (y up): moves up
    const Rect three = autoresize(f, from, to, 1 | 2 | 4);                                // the extra width is shared in three
    CHECK(std::fabs(three.origin.x - (10 + 100.0 / 3)) < 1e-9 && std::fabs(three.size.width - (100 + 100.0 / 3)) < 1e-9);
}
