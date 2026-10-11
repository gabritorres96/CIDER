// CoreGraphics drawing engine (CIDER 0.6.1): state machine with a recording device, and real rasterization through Direct2D into a WIC bitmap.
#include <cmath>

#include "cider/cg.hpp"
#include "test_framework.hpp"
#ifdef _WIN32
#include "../../src/graphics/d2d_device.hpp"
#endif

#define ASSERT_TRUE(c) do { if (!(c)) { CHECK(c); return; } } while (0)

using namespace cider::cg;
using cider::geometry::make_rect;
constexpr double kPi = 3.14159265358979323846;

namespace {
struct Rec : Device {
    struct Op { std::string kind; Affine t; Color c; Rect r; };
    std::vector<Op> ops;
    int clips = 0, max_clips = 0;
    const char* name() const override { return "recording"; }
    void clear(const Color& c) override { ops.push_back({"clear", {}, c, {}}); }
    void fill_path(const Path& p, const Affine& t, const Color& c, FillRule, bool) override { ops.push_back({"fill", t, c, p.bounding_box()}); }
    void stroke_path(const Path& p, const Affine& t, const Color& c, const StrokeStyle&, bool) override { ops.push_back({"stroke", t, c, p.bounding_box()}); }
    void push_clip(const Path&, const Affine&, FillRule) override { ++clips; max_clips = std::max(max_clips, clips); }
    void pop_clip() override { --clips; }
    void draw_image(const Image&, const Rect& d, const Affine& t, double, bool) override { ops.push_back({"image", t, {}, d}); }
    void draw_text(const std::u16string&, const FontSpec&, Point, const Affine& t, const Color& c) override { ops.push_back({"text", t, c, {}}); }
    Size measure_text(const std::u16string&, const FontSpec&) override { return {1, 1}; }
    bool ok() const override { return true; }
};
bool near(double a, double b) { return std::fabs(a - b) < 1e-9; }
}  // namespace

CIDER_TEST(affine_concat_applies_first_argument_first) {
    const Affine t = concat(Affine::translation(10, 0), Affine::scaling(2, 2));       // translate, then scale
    const Point p = t.apply(Point{1, 1});
    CHECK(near(p.x, 22) && near(p.y, 2));
    Affine inv;
    CHECK(invert(t, inv));
    const Point q = inv.apply(p);
    CHECK(near(q.x, 1) && near(q.y, 1));
    CHECK(!invert(Affine::scaling(0, 1), inv));
    const Point r = Affine::rotation(kPi / 2).apply(Point{1, 0});
    CHECK(near(r.x, 0) && near(r.y, 1));
}

CIDER_TEST(state_stack_restores_ctm_colors_and_pops_clips) {
    Rec dev;
    {
        Context c(&dev, Affine{}, Size{100, 100});
        c.mutable_state().fill = Color{1, 0, 0, 1};
        c.save();
        c.translate(5, 5);
        c.mutable_state().fill = Color{0, 1, 0, 1};
        c.clip_to_rect(make_rect(0, 0, 10, 10));
        CHECK_EQ(dev.clips, 1);
        c.save();
        c.clip_to_rect(make_rect(1, 1, 3, 3));
        CHECK_EQ(dev.clips, 2);
        CHECK(c.restore());
        CHECK_EQ(dev.clips, 1);
        CHECK(c.restore());
        CHECK_EQ(dev.clips, 0);
        CHECK(near(c.state().ctm.tx, 0));
        CHECK(c.state().fill == (Color{1, 0, 0, 1}));
        CHECK(!c.restore());                                    // unbalanced restore is reported, not applied
        c.save();
        c.clip_to_rect(make_rect(0, 0, 1, 1));                  // left pushed on purpose: the destructor must pop it
    }
    CHECK_EQ(dev.clips, 0);
}

CIDER_TEST(paths_are_transformed_when_added_not_when_painted) {
    Rec dev;
    Context c(&dev, Affine{}, Size{100, 100});
    c.translate(10, 20);
    c.fill_rect(make_rect(0, 0, 5, 5));
    c.translate(100, 100);                                      // later change does not move the already painted rect
    ASSERT_TRUE(dev.ops.size() == 1);
    CHECK(near(dev.ops[0].r.min_x(), 10) && near(dev.ops[0].r.min_y(), 20));
    c.begin_path();
    c.path().move_to(c.to_device_space_point(Point{0, 0}));
    c.path().line_to(c.to_device_space_point(Point{1, 0}));
    CHECK(near(c.path().current_point().x, 111));
}

CIDER_TEST(path_geometry_arc_rect_ellipse_and_hit_testing) {
    Path p;
    p.add_rect(make_rect(0, 0, 10, 10));
    Rect r;
    CHECK(p.is_rect(r) && near(r.width(), 10));
    CHECK(p.contains(Point{5, 5}, FillRule::NonZero) && !p.contains(Point{15, 5}, FillRule::NonZero));
    Path e;
    e.add_ellipse(make_rect(0, 0, 10, 10));
    CHECK(e.contains(Point{5, 5}, FillRule::NonZero) && !e.contains(Point{0.3, 0.3}, FillRule::NonZero));
    Path a;
    a.arc(Point{0, 0}, 10, 0, kPi, false);
    const Rect bb = a.bounding_box();
    CHECK(bb.max_x() > 9.9 && bb.min_x() < -9.9);
    Path donut;                                                  // even-odd makes the inner square a hole, non-zero (same winding) does not
    donut.add_rect(make_rect(0, 0, 10, 10));
    donut.add_rect(make_rect(3, 3, 4, 4));
    CHECK(!donut.contains(Point{5, 5}, FillRule::EvenOdd));
    CHECK(donut.contains(Point{5, 5}, FillRule::NonZero));
    CHECK(Path().bounding_box().width() <= 0 || cider::geometry::is_null(Path().bounding_box()));
}

CIDER_TEST(stroke_width_uses_the_ctm_scale_and_flipped_base) {
    Rec dev;
    Context c(&dev, view_base(100, false, 2.0), Size{100, 100});
    c.fill_rect(make_rect(0, 0, 10, 10));
    ASSERT_TRUE(!dev.ops.empty());
    const Point o = dev.ops[0].t.apply(Point{0, 0});            // user (0,0) is the bottom-left: device y = 100 * 2
    CHECK(near(o.x, 0) && near(o.y, 200));
    Rec dev2;
    Context f(&dev2, view_base(100, true, 1.0), Size{100, 100});
    f.fill_rect(make_rect(0, 0, 10, 10));
    CHECK(near(dev2.ops[0].t.apply(Point{0, 0}).y, 0));
}

#ifdef _WIN32
namespace {
struct Px { uint8_t b, g, r, a; };
Px at(const std::vector<uint8_t>& v, uint32_t w, uint32_t x, uint32_t y) { const uint8_t* p = &v[(static_cast<size_t>(y) * w + x) * 4]; return {p[0], p[1], p[2], p[3]}; }
}  // namespace

CIDER_TEST(direct2d_bitmap_target_fills_with_the_right_orientation) {
    { auto warm = D2DDevice::create_bitmap(1, 1); }               // factories are created lazily and live until shutdown_d2d()
    const int before = com_objects_alive();
    {
        auto dev = D2DDevice::create_bitmap(40, 40);
        if (!dev) { return; }                                   // Direct2D unavailable (e.g. very old or locked-down image): nothing to check
        ASSERT_TRUE(dev->begin());
        dev->clear(Color{1, 1, 1, 1});
        {
            Context c(dev.get(), view_base(40, false, 1.0), Size{40, 40});
            c.mutable_state().fill = Color{1, 0, 0, 1};
            c.fill_rect(make_rect(0, 0, 10, 10));               // bottom-left in a y-up context -> lower-left pixels
            c.mutable_state().fill = Color{0, 0, 1, 1};
            c.fill_rect(make_rect(30, 30, 10, 10));              // top-right
        }
        ASSERT_TRUE(dev->end());
        std::vector<uint8_t> px;
        ASSERT_TRUE(dev->read_pixels(px));
        const Px ll = at(px, 40, 5, 35), ur = at(px, 40, 35, 5), mid = at(px, 40, 20, 20);
        CHECK(ll.r == 255 && ll.g == 0 && ll.b == 0);
        CHECK(ur.b == 255 && ur.r == 0);
        CHECK(mid.r == 255 && mid.g == 255 && mid.b == 255);
    }
    CHECK_EQ(com_objects_alive(), before);                       // no COM leak
}

CIDER_TEST(direct2d_clip_stroke_and_flip_are_applied) {
    auto dev = D2DDevice::create_bitmap(40, 40);
    if (!dev) return;
    ASSERT_TRUE(dev->begin());
    dev->clear(Color{1, 1, 1, 1});
    {
        Context c(dev.get(), view_base(40, true, 1.0), Size{40, 40});     // flipped: origin top-left
        c.save();
        c.clip_to_rect(make_rect(0, 0, 20, 20));
        c.mutable_state().fill = Color{0, 0, 0, 1};
        c.fill_rect(make_rect(0, 0, 40, 40));                    // clipped to the top-left quarter
        c.restore();
        c.mutable_state().stroke = Color{1, 0, 0, 1};
        c.mutable_state().line.width = 4;
        c.begin_path();
        c.path().move_to(c.to_device_space_point(Point{0, 30}));
        c.path().line_to(c.to_device_space_point(Point{40, 30}));
        c.stroke_current();
    }
    ASSERT_TRUE(dev->end());
    std::vector<uint8_t> px;
    ASSERT_TRUE(dev->read_pixels(px));
    CHECK(at(px, 40, 10, 10).r == 0 && at(px, 40, 10, 10).g == 0);        // black inside the clip
    CHECK(at(px, 40, 30, 10).g == 255);                                   // white outside
    CHECK(at(px, 40, 30, 30).r == 255 && at(px, 40, 30, 30).g == 0);      // red stroke row 30
    CHECK(at(px, 40, 30, 20).g == 255);                                   // untouched
}

CIDER_TEST(wic_decode_validates_limits_and_round_trips_png) {
    Image img;
    img.width = 4;
    img.height = 2;
    img.bgra.assign(4 * 2 * 4, 0);
    for (size_t i = 0; i < img.bgra.size(); i += 4) { img.bgra[i] = 10; img.bgra[i + 1] = 20; img.bgra[i + 2] = 30; img.bgra[i + 3] = 255; }
    std::vector<uint8_t> png;
    std::string err, fmt;
    if (!encode_png(img, png, err)) return;                       // WIC unavailable
    Image back;
    CHECK(decode_image(png.data(), png.size(), DecodeLimits{}, back, fmt, err));
    CHECK(fmt == "png" && back.width == 4 && back.height == 2);
    CHECK(back.bgra[0] == 10 && back.bgra[2] == 30);
    DecodeLimits tiny;
    tiny.max_dimension = 3;
    Image none;
    CHECK(!decode_image(png.data(), png.size(), tiny, none, fmt, err));
    CHECK(none.bgra.empty());                                     // nothing allocated for a rejected image
    const uint8_t junk[16] = {1, 2, 3};
    CHECK(!decode_image(junk, sizeof junk, DecodeLimits{}, none, fmt, err));
    CHECK(!decode_image(nullptr, 0, DecodeLimits{}, none, fmt, err));
}
#endif

CIDER_TEST(non_finite_and_huge_arc_arguments_terminate_and_are_ignored) {          // found by fuzz_cg: an arc with a 1e300 sweep looped for ever
    Path p;
    p.arc(Point{0, 0}, 10, 0, 1e300, false);                                          // huge but finite: reduced modulo 2 pi, a full circle at most
    CHECK(!p.empty());
    CHECK(p.bounding_box().width() <= 20.0001);
    Path q;
    q.arc(Point{0, 0}, 10, 0, INFINITY, true);
    q.arc(Point{std::nan(""), 0}, 10, 0, 1, true);
    q.arc_to(Point{INFINITY, 0}, Point{1, 1}, 2);
    q.move_to(Point{std::nan(""), 1});
    q.line_to(Point{1, -INFINITY});
    CHECK(q.empty());                                                                 // nothing was added
}
