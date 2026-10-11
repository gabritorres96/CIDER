// CoreGraphics from a Mach-O guest (CIDER 0.6.1 / 0.6.2): cg_draw_test is a real Objective-C program compiled for macOS; its -drawRect: draws a scene with CoreGraphics
// through the CIDER bridge onto a Direct2D bitmap surface of the offscreen backend. The PNG snapshots of the window are decoded and compared pixel by pixel.
#include <cmath>
#include <filesystem>

#include "../../src/graphics/cg_api.hpp"
#include "../../src/graphics/d2d_device.hpp"
#include "fixture_run.hpp"

#define ASSERT_TRUE_OR_RETURN(c) do { if (!(c)) { CHECK(c); return; } } while (0)

using namespace cider::testing;
namespace fs = std::filesystem;

namespace {

struct Shot {
    cider::cg::Image img;
    bool ok = false;
};

Shot load(const fs::path& file) {
    Shot s;
    auto bytes = cider::read_file(file, 16u << 20);
    if (!bytes.ok()) return s;
    std::string fmt, err;
    s.ok = cider::cg::decode_image(bytes.value().data(), bytes.value().size(), cider::cg::DecodeLimits{}, s.img, fmt, err) && fmt == "png";
    return s;
}
struct Px { int r, g, b, a; };
Px at(const Shot& s, uint32_t x, uint32_t y) {
    const uint8_t* p = &s.img.bgra[(static_cast<size_t>(y) * s.img.width + x) * 4];
    return {p[2], p[1], p[0], p[3]};
}
bool is(const Shot& s, uint32_t x, uint32_t y, int r, int g, int b, int tol = 3) {
    const Px p = at(s, x, y);
    return std::abs(p.r - r) <= tol && std::abs(p.g - g) <= tol && std::abs(p.b - b) <= tol && p.a == 255;
}
size_t count_not(const Shot& s, int r, int g, int b, int tol = 3) {
    size_t n = 0;
    for (uint32_t y = 0; y < s.img.height; ++y) for (uint32_t x = 0; x < s.img.width; ++x) if (!is(s, x, y, r, g, b, tol)) ++n;
    return n;
}

fs::path tmpdir() {
    static const fs::path d = [] {
        fs::path p = fs::temp_directory_path() / "cider_cg_rendering_test";
        fs::create_directories(p);
        return p;
    }();
    return d;
}
std::string P(const char* name) { return cider::path_to_utf8(tmpdir() / name); }

FixtureRun run_scene(std::vector<std::string> script, std::vector<std::string> args = {}) {
    FixtureOptions o;
    o.gui_backend = "offscreen";
    o.gui_script = std::move(script);
    o.args = std::move(args);
    return run_fixture_file(appkit_fixture("cg_draw_test"), o);
}

// BG = (255, 255, 204). Unflipped 400x300 view at the top of the 400x340 window: view pixel (x, py) is user (x, 300 - py) at scale 1.
void check_scene_unflipped(const Shot& s, bool second_state, double k = 1.0, uint32_t view_h = 300) {
    auto X = [&](double x) { return static_cast<uint32_t>(std::lround(x * k)); };
    auto Y = [&](double user_y) { return static_cast<uint32_t>(std::lround((view_h - user_y) * k)); };
    CHECK(is(s, X(10), Y(290), 255, 255, 204));                                       // solid background (top-left corner area)
    CHECK(is(s, X(60), Y(40), second_state ? 0 : 255, 0, second_state ? 255 : 0));    // state rectangle: red, then blue after the button
    CHECK(is(s, X(340), Y(260), 0, 0, 255));                                          // blue rectangle (top-right: a vertical inversion would land at the bottom)
    CHECK(is(s, X(340), Y(40), 255, 255, 204));                                       // ... and the bottom-right is background
    const Px line = at(s, X(200), Y(80));                                              // diagonal line: black
    CHECK(line.r < 40 && line.g < 40 && line.b < 40);
    CHECK(is(s, X(160), Y(80), 255, 255, 204));                                       // beside the line (above-left of it)
    CHECK(is(s, X(210), Y(190), 0, 255, 0));                                          // ellipse centre: green
    CHECK(is(s, X(155), Y(152), 255, 255, 204));                                      // inside the ellipse's bounding box corner: NOT filled
    CHECK(is(s, X(60), Y(120), 255, 128, 0));                                         // triangle (CGPath)
    CHECK(is(s, X(35), Y(150), 255, 255, 204));                                       // outside the triangle (its top is at x = 60)
    CHECK(is(s, X(320), Y(100), 128, 0, 128));                                        // rotated square centre
    CHECK(is(s, X(345), Y(100), 128, 0, 128));                                        // inside the 45-degree diamond, outside the unrotated square
    CHECK(is(s, X(338), Y(118), 255, 255, 204));                                      // inside the unrotated square, outside the diamond
    CHECK(is(s, X(180), Y(270), 0, 255, 255));                                        // clipped cyan: inside the clip
    CHECK(is(s, X(110), Y(270), 255, 255, 204));                                      // inside the cyan rectangle but outside the clip: untouched
    CHECK(is(s, X(110), Y(220), 255, 255, 204));
    const Px edge = at(s, X(310), Y(175));                                            // stroked rectangle: left edge, 6 wide centred on x = 310
    CHECK(edge.r < 40 && edge.g < 40 && edge.b < 40);
    CHECK(is(s, X(340), Y(175), 255, 255, 204));                                      // interior untouched
    CHECK(is(s, X(300), Y(175), 255, 255, 204));                                      // outside untouched
    CHECK(count_not(s, 255, 255, 204) > 3000 * static_cast<size_t>(k * k));            // the scene is really there: the image is not blank
}

}  // namespace

CIDER_TEST(guest_drawrect_renders_the_scene_with_correct_orientation_and_the_redraw_button_changes_it) {
    const std::string a = P("a.png"), b = P("b.png");
    std::filesystem::remove(a);
    std::filesystem::remove(b);
    FixtureRun r = run_scene({"wait 10", "snapshot " + a, "click Redraw", "snapshot " + b, "close"});
    expect_exit_code(r, 0, "cg_draw_test");
    CHECK(r.err.find("cg_draw_test done: draws=2 redraws=1 failures=0") != std::string::npos);
    CHECK(r.err.find("FAIL") == std::string::npos);
    CHECK_EQ(r.r.gui_native_objects_after, uint64_t{0});
    CHECK_EQ(r.r.objc_live_summary, std::string());
    CHECK_EQ(r.r.objc_live_objects, uint64_t{0});                  // views, contexts, colors, paths: nothing the guest or the host created is left
    const Shot s1 = load(a), s2 = load(b);
    ASSERT_TRUE_OR_RETURN(s1.ok && s2.ok);
    CHECK_EQ(s1.img.width, 400u);
    CHECK_EQ(s1.img.height, 340u);
    check_scene_unflipped(s1, false);
    check_scene_unflipped(s2, true);
}

CIDER_TEST(flipped_view_draws_the_same_commands_mirrored) {
    const std::string a = P("flip.png");
    std::filesystem::remove(a);
    FixtureRun r = run_scene({"wait 10", "snapshot " + a, "close"}, {"flipped"});
    expect_exit_code(r, 0, "cg_draw_test flipped");
    CHECK(r.err.find("failures=0") != std::string::npos);
    const Shot s = load(a);
    ASSERT_TRUE_OR_RETURN(s.ok);
    // flipped: user (x, y) is pixel (x, y) from the top of the view
    CHECK(is(s, 60, 40, 255, 0, 0));                               // red rectangle at the TOP-left now
    CHECK(is(s, 340, 260, 0, 0, 255));                             // blue rectangle at the bottom-right
    CHECK(is(s, 210, 190, 0, 255, 0));                             // ellipse
    CHECK(is(s, 60, 120, 255, 128, 0));                            // triangle
    CHECK(is(s, 320, 100, 128, 0, 128));
    CHECK(is(s, 180, 270, 0, 255, 255));
    CHECK(is(s, 110, 270, 255, 255, 204));
}

CIDER_TEST(resize_repaints_at_the_new_size_with_autoresized_bounds) {
    const std::string a = P("resized.png");
    std::filesystem::remove(a);
    FixtureRun r = run_scene({"wait 10", "resize 600 440", "snapshot " + a, "close"});
    expect_exit_code(r, 0, "cg_draw_test resize");
    CHECK(r.err.find("failures=0") != std::string::npos);                    // the dirty rectangle equalled the new bounds on every pass
    const Shot s = load(a);
    ASSERT_TRUE_OR_RETURN(s.ok);
    CHECK_EQ(s.img.width, 600u);
    CHECK_EQ(s.img.height, 440u);
    // the view is now 600x400; the scene keeps its absolute user coordinates, so everything sits relative to the bottom of the view
    CHECK(is(s, 550, 30, 255, 255, 204));                                    // the new area is painted with the background
    CHECK(is(s, 60, 400 - 40, 255, 0, 0));                                   // red rectangle moved with the bottom edge
    CHECK(is(s, 340, 400 - 260, 0, 0, 255));
    CHECK(is(s, 210, 400 - 190, 0, 255, 0));
}

CIDER_TEST(dpi_scale_two_renders_twice_as_many_pixels_with_scaled_strokes) {
    const std::string a = P("scale2.png");
    std::filesystem::remove(a);
    FixtureRun r = run_scene({"wait 10", "scale 2", "snapshot " + a, "close"});
    expect_exit_code(r, 0, "cg_draw_test scale");
    CHECK(r.err.find("failures=0") != std::string::npos);
    const Shot s = load(a);
    ASSERT_TRUE_OR_RETURN(s.ok);
    CHECK_EQ(s.img.width, 800u);
    CHECK_EQ(s.img.height, 680u);
    check_scene_unflipped(s, false, 2.0);
    // the 6-point stroke is 12 pixels wide: pixel columns 619..631 around x = 620 are dark, 616 is not
    const Px in = at(s, 620 - 5, 2 * (300 - 175)), out = at(s, 620 - 9, 2 * (300 - 175));
    CHECK(in.r < 40);
    CHECK(out.r > 200);
}

CIDER_TEST(repeated_repaints_do_not_leak_surfaces_contexts_or_objects) {
    std::vector<std::string> script = {"wait 10"};
    for (int i = 0; i < 120; ++i) script.push_back("click Redraw");
    script.push_back("close");
    { auto warm = cider::cg::D2DDevice::create_bitmap(1, 1); }               // the factories are created lazily and live until shutdown
    const int com_before = cider::cg::com_objects_alive();
    FixtureRun r = run_scene(script);
    expect_exit_code(r, 0, "cg_draw_test repeated");
    CHECK(r.err.find("redraws=120 failures=0") != std::string::npos);
    CHECK_EQ(r.r.objc_live_objects, uint64_t{0});
    CHECK_EQ(r.r.gui_native_objects_after, uint64_t{0});
    CHECK_EQ(cider::cg::com_objects_alive(), com_before);                     // every Direct2D / WIC object created by 120+ frames was released
}

CIDER_TEST(window_cycles_close_during_drawrect_stale_contexts_and_self_invalidation_are_safe) {
    { auto warm = cider::cg::D2DDevice::create_bitmap(1, 1); }
    const int com_before = cider::cg::com_objects_alive();
    const uint64_t invalid_before = cider::graphics::cg_invalid_handle_events();
    FixtureOptions o;
    o.gui_backend = "offscreen";
    FixtureRun r = run_fixture_file(appkit_fixture("cg_lifecycle"), o);
    expect_exit_code(r, 0, "cg_lifecycle");
    CHECK(r.err.find("FAIL") == std::string::npos);
    CHECK(r.err.find("cg_lifecycle done: failures=0\n") != std::string::npos);
    CHECK_EQ(cider::graphics::cg_invalid_handle_events() - invalid_before, uint64_t{2});     // the two calls made with the stale context
    CHECK_EQ(r.r.objc_live_summary, std::string());
    CHECK_EQ(r.r.objc_live_objects, uint64_t{0});
    CHECK_EQ(r.r.gui_native_objects_after, uint64_t{0});
    CHECK_EQ(r.r.gui_open_windows_at_exit, uint64_t{0});
    CHECK_EQ(cider::cg::com_objects_alive(), com_before);
    CHECK_EQ(cider::graphics::cg_live_objects(), size_t{0});
}

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

// The same life-cycle program on REAL windows (Win32 backend, in this process): HWNDs, Direct2D window render targets, WM_PAINT, closing during a repaint.
// GDI and USER handles of this process are counted around a second run (the first one warms up the Direct2D / DirectWrite factories).
CIDER_TEST(real_windows_cycle_without_leaking_gdi_user_handles_or_com_objects) {
    HDESK d = OpenInputDesktop(0, FALSE, DESKTOP_SWITCHDESKTOP);
    if (!d || std::getenv("CIDER_NO_GUI_TESTS")) { if (d) CloseDesktop(d); SKIP("no interactive desktop"); }
    CloseDesktop(d);
    auto run_once = [&] {
        FixtureRun r = run_fixture_file(appkit_fixture("cg_lifecycle"));                    // default backend: win32
        expect_exit_code(r, 0, "cg_lifecycle (win32)");
        CHECK(r.err.find("cg_lifecycle done: failures=0\n") != std::string::npos);
        CHECK_EQ(r.r.gui_native_objects_after, uint64_t{0});
        CHECK_EQ(r.r.objc_live_objects, uint64_t{0});
    };
    run_once();
    const DWORD gdi_before = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS), user_before = GetGuiResources(GetCurrentProcess(), GR_USEROBJECTS);
    const int com_before = cider::cg::com_objects_alive();
    run_once();
    const DWORD gdi_after = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS), user_after = GetGuiResources(GetCurrentProcess(), GR_USEROBJECTS);
    CHECK(gdi_after <= gdi_before);                                  // 40 windows with Direct2D surfaces, repainted and closed: not one GDI object left behind
    CHECK(user_after <= user_before);
    CHECK_EQ(cider::cg::com_objects_alive(), com_before);
}
#endif
